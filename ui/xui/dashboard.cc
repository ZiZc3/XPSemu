//
// XPSemu: the dashboard (the PS5's front end for xemu)
//
// A full screen scene in the spirit of the original console's dashboard: a
// dark green backdrop with a fine curved grid, a large glowing orb, and the
// menu as angled green bars; the games in /data/xemu/games (general.
// games_dir), simple settings and system information as pages. It's drawn
// with a small number of ImGui draw list primitives (no textures, and no
// assets from the original dashboard), so it stays light at 4K.
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
#include <time.h>

#include "common.hh"
#include "actions.hh"
#include "dashboard.hh"
#include "font-manager.hh"
#include "input-manager.hh"
#include "main-menu.hh"
#include "scene-manager.hh"
#include "game-log.hh"
#include "game-profile.hh"
#include "ui-sounds.hh"
#include "xiso.hh"
#include "../xemu-notifications.h"
#include "../../xemu-xbe.h"
#include "../../hw/xbox/xemu-eeprom.h"
#include "../xemu-input.h"
#include "../xemu-settings.h"
#include "xemu-version.h"

DashboardScene g_dashboard;

//
// Look: colours of the original dashboard, dark green to yellow-green.
//

static ImU32 Rgba(int r, int g, int b, float a)
{
    return IM_COL32(std::clamp(r, 0, 255), std::clamp(g, 0, 255),
                    std::clamp(b, 0, 255),
                    (int)(255 * std::clamp(a, 0.f, 1.f)));
}

static ImU32 Line(float a) { return Rgba(70, 170, 30, a); }       // Outlines
static ImU32 Label(float a) { return Rgba(150, 235, 60, a); }     // Text
static ImU32 Lime(float a) { return Rgba(205, 245, 45, a); }      // Selection
static ImU32 Ink(float a) { return Rgba(12, 45, 6, a); }          // On lime
static ImU32 White(float a) { return Rgba(255, 255, 255, a); }

static float Approach(float value, float target, float dt, float speed = 12)
{
    return value + (target - value) * (1 - expf(-speed * dt));
}

static void Text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos,
                 ImU32 col, const char *text, float wrap = 0)
{
    dl->AddText(font, size, pos, col, text, NULL, wrap);
}

static ImVec2 TextSize(ImFont *font, float size, const char *text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0, text);
}

static std::string Upper(const char *text)
{
    std::string s(text);
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

// An angled bar like the original menu's: corners cut top left and bottom
// right.
static void BarPath(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float cut)
{
    dl->PathLineTo(ImVec2(p0.x + cut, p0.y));
    dl->PathLineTo(ImVec2(p1.x, p0.y));
    dl->PathLineTo(ImVec2(p1.x, p1.y - cut));
    dl->PathLineTo(ImVec2(p1.x - cut, p1.y));
    dl->PathLineTo(ImVec2(p0.x, p1.y));
    dl->PathLineTo(ImVec2(p0.x, p0.y + cut));
}

static void Bar(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float s, bool selected,
                float a)
{
    float cut = (p1.y - p0.y) * 0.3f;
    BarPath(dl, p0, p1, cut);
    dl->PathFillConvex(selected ? Lime(a * 0.95f) : Rgba(10, 40, 6, a * 0.7f));
    BarPath(dl, p0, p1, cut);
    dl->PathStroke(selected ? Lime(a) : Line(a * 0.9f), ImDrawFlags_Closed,
                   2 * s);
}

// The PS5's own focus cue: a white outline with a soft glow, pulsing gently.
static void Focus(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float s, float t,
                  float a)
{
    float pulse = 0.75f + 0.25f * sinf(t * 4);
    float pad = 6 * s;
    for (int i = 3; i >= 1; i--) {
        float g = pad + i * 4 * s;
        dl->AddRect(ImVec2(p0.x - g, p0.y - g), ImVec2(p1.x + g, p1.y + g),
                    White(a * 0.07f * pulse), 14 * s, 0, 4 * s);
    }
    dl->AddRect(ImVec2(p0.x - pad, p0.y - pad), ImVec2(p1.x + pad, p1.y + pad),
                White(a * pulse), 12 * s, 0, 3.5f * s);
}

// The controller's face buttons, drawn (so no special font is needed).
enum Glyph { GLYPH_CROSS, GLYPH_CIRCLE, GLYPH_TRIANGLE, GLYPH_SQUARE };

static void DrawGlyph(ImDrawList *dl, ImVec2 c, float r, Glyph g, float a)
{
    ImU32 col = White(a);
    float t = r * 0.14f;
    dl->AddCircle(c, r, White(a * 0.6f), 32, t * 0.7f);
    float k = r * 0.45f;
    switch (g) {
    case GLYPH_CROSS:
        dl->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), col, t);
        dl->AddLine(ImVec2(c.x - k, c.y + k), ImVec2(c.x + k, c.y - k), col, t);
        break;
    case GLYPH_CIRCLE:
        dl->AddCircle(c, k, col, 24, t);
        break;
    case GLYPH_TRIANGLE:
        dl->AddTriangle(ImVec2(c.x, c.y - k), ImVec2(c.x + k, c.y + k * 0.75f),
                        ImVec2(c.x - k, c.y + k * 0.75f), col, t);
        break;
    case GLYPH_SQUARE:
        dl->AddRect(ImVec2(c.x - k * 0.8f, c.y - k * 0.8f),
                    ImVec2(c.x + k * 0.8f, c.y + k * 0.8f), col, 0, 0, t);
        break;
    }
}

//
// Game art: the title image inside each game (default.xbe's $$XTIMAGE), and
// box art the user adds as pictures in games_dir/covers, named after the
// game's file or its title ID (e.g. "Halo.jpg" or "4D530004.png"). Read once
// per disc image; turned into textures as they come into view, a few per
// frame so scrolling doesn't stall.
//

struct GameArt {
    bool read = false;
    XisoInfo info;
    std::string stem;
    XemuTexture image = 0, cover = 0;
    bool image_tried = false, cover_tried = false;
};

static std::map<std::string, GameArt> g_art; // By disc image path
static int g_texture_budget;                  // Textures left this frame

static GameArt &Art(const std::string &path)
{
    return g_art[path];
}

// Loads a cover picture, shrunk to at most max_h rows (box art at 4K
// doesn't need more, and a few dozen stay small).
static XemuTexture LoadCover(const std::string &file, int max_h)
{
    int w, h, channels;
    unsigned char *data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    if (!data) {
        return 0;
    }
    XemuTexture tex;
    if (h > max_h) {
        // Box filter: each output pixel averages the input pixels under it.
        int ow = std::max(1, w * max_h / h), oh = max_h;
        std::vector<uint8_t> out((size_t)ow * oh * 4);
        for (int y = 0; y < oh; y++) {
            int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
            for (int x = 0; x < ow; x++) {
                int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
                uint32_t sum[4] = { 0 }, n = 0;
                for (int sy = y0; sy < y1; sy++) {
                    const unsigned char *p = data + ((size_t)sy * w + x0) * 4;
                    for (int sx = x0; sx < x1; sx++, p += 4, n++) {
                        for (int c = 0; c < 4; c++) {
                            sum[c] += p[c];
                        }
                    }
                }
                for (int c = 0; c < 4; c++) {
                    out[((size_t)y * ow + x) * 4 + c] = sum[c] / n;
                }
            }
        }
        tex = xemu_vk_texture_create(out.data(), ow, oh, 4);
    } else {
        tex = xemu_vk_texture_create(data, w, h, 4);
    }
    stbi_image_free(data);
    return tex;
}

static XemuTexture GameImage(const DashboardScene::Game &game)
{
    GameArt &art = Art(game.path);
    if (!art.image_tried && g_texture_budget > 0) {
        art.image_tried = true;
        if (!art.info.image.empty()) {
            g_texture_budget--;
            art.image = xemu_vk_texture_create(art.info.image.data(),
                                               art.info.image_width,
                                               art.info.image_height, 4);
            art.info.image.clear(); // The texture has it now
            art.info.image.shrink_to_fit();
        }
    }
    return art.image;
}

// Cover pictures by name key (see CoverKey), from games_dir/covers and
// /data/xemu/covers; listed again each time the dashboard opens, so covers
// added while XPSemu runs show up.
static std::map<std::string, std::string> g_covers;

// A name reduced to what matters for matching: lower case letters and
// digits, without parts in brackets, so "Halo 2 (USA)", "Halo 2" and
// "halo2" all give "halo2".
static std::string CoverKey(const std::string &name)
{
    std::string key;
    int depth = 0;
    for (unsigned char c : name) {
        if (c == '(' || c == '[') {
            depth++;
        } else if ((c == ')' || c == ']') && depth > 0) {
            depth--;
        } else if (!depth && isalnum(c)) {
            key += (char)tolower(c);
        }
    }
    return key;
}

static std::string KeyOf(const DashboardScene::Game &game)
{
    std::string fallback = CoverKey(Art(game.path).stem);
    return GameKey(game.title_id, fallback.empty() ? "game" : fallback);
}

static std::string CleanName(const std::string &stem)
{
    std::string name;
    int depth = 0;
    for (char c : stem) {
        if (c == '(' || c == '[') {
            depth++;
        } else if ((c == ')' || c == ']') && depth > 0) {
            depth--;
        } else if (!depth) {
            name += c == '_' ? ' ' : c;
        }
    }
    // Single spaces, none at the ends.
    std::string out;
    for (char c : name) {
        if (c != ' ' || (!out.empty() && out.back() != ' ')) {
            out += c;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '-')) {
        out.pop_back();
    }
    return out.empty() ? stem : out;
}

// Games' own settings, read once (kept up to date by ChangeGameSetting).
static std::map<std::string, GameProfile> g_profiles;

static const GameProfile &ProfileOf(const DashboardScene::Game &game)
{
    std::string key = KeyOf(game);
    auto it = g_profiles.find(key);
    if (it == g_profiles.end()) {
        it = g_profiles.emplace(key, GameProfileLoad(key)).first;
    }
    return it->second;
}

static void ListCovers()
{
    g_covers.clear();
    std::string dirs[] = { std::string(g_config.general.games_dir) + "/covers",
                           "/data/xemu/covers" };
    std::error_code ec;
    for (const auto &dir : dirs) {
        if (!std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        for (const auto &entry :
             std::filesystem::directory_iterator(dir, ec)) {
            std::string ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (entry.is_regular_file(ec) &&
                (ext == ".jpg" || ext == ".jpeg" || ext == ".png")) {
                g_covers.emplace(CoverKey(entry.path().stem().string()),
                                 entry.path().string());
            }
        }
    }
}

static XemuTexture GameCover(const DashboardScene::Game &game)
{
    GameArt &art = Art(game.path);
    if (!art.cover_tried && g_texture_budget > 0) {
        art.cover_tried = true;
        std::vector<std::string> keys = { CoverKey(art.stem),
                                          CoverKey(game.name),
                                          CoverKey(art.info.title) };
        if (game.title_id) {
            char id[16];
            snprintf(id, sizeof(id), "%08x", game.title_id);
            keys.push_back(id);
        }
        for (const auto &key : keys) {
            auto it = g_covers.find(key);
            if (key.empty() || it == g_covers.end()) {
                continue;
            }
            g_texture_budget--;
            art.cover = LoadCover(it->second, 720);
            fprintf(stderr, "XPSemu: cover for %s: %s%s%s\n",
                    game.name.c_str(), it->second.c_str(),
                    art.cover ? "" : " FAILED: ",
                    art.cover ? "" : stbi_failure_reason());
            return art.cover;
        }
    }
    return art.cover;
}

//
// Scene
//

void DashboardScene::ScanGames()
{
    m_games.clear();
    ListCovers();
    for (auto &[path, art] : g_art) {
        if (!art.cover) {
            art.cover_tried = false;
        }
    }
    const char *dir = g_config.general.games_dir;
    std::error_code ec;
    if (dir && dir[0] && std::filesystem::is_directory(dir, ec)) {
        std::filesystem::create_directory(std::string(dir) + "/covers", ec);
        for (const auto &entry :
             std::filesystem::directory_iterator(dir, ec)) {
            const auto &path = entry.path();
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (entry.is_regular_file(ec) && (ext == ".iso" || ext == ".xiso")) {
                GameArt &art = Art(path.string());
                if (!art.read) {
                    art.read = true;
                    art.stem = path.stem().string();
                    XisoReadInfo(path.string().c_str(), &art.info);
                }
                m_games.push_back({ CleanName(art.stem), path.string(),
                                    art.info.title_id, art.info.full_disc });
            }
        }
    }
    std::sort(m_games.begin(), m_games.end(), [](const Game &a, const Game &b) {
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });

    // Start on the game that's in the drive, if it's one of these.
    const char *dvd = g_config.sys.files.dvd_path;
    for (size_t i = 0; dvd && i < m_games.size(); i++) {
        if (m_games[i].path == dvd) {
            m_game = i;
        }
    }
    m_game = std::clamp(m_game, 0, std::max(0, (int)m_games.size() - 1));
    m_game_anim = m_game;
}

void DashboardScene::Show()
{
    if (!m_recent_loaded) {
        LoadRecent();
    }
    PatchesRescan();
    ScanGames();
    m_visible = true;
    m_in_page = false;
    m_advanced = false;
    m_shelf = false;
    m_gs_open = false;
    m_gs_patches = false;
    m_page_anim = 0;
    // The press that opened the dashboard mustn't also act in it.
    m_prev_buttons = g_input_mgr.CombinedButtons();
    UiSoundPlay(UI_SOUND_OPEN);
    // Hold the game (or the Xbox dashboard) while this one is up.
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
        m_paused_vm = true;
    }
}

void DashboardScene::Hide()
{
    m_visible = false;
    if (m_paused_vm) {
        m_paused_vm = false;
        vm_start();
    }
}

bool DashboardScene::IsAnimating()
{
    return m_visible ? m_alpha < 1 : m_alpha > 0;
}

void DashboardScene::Close()
{
    if (m_can_return) {
        UiSoundPlay(UI_SOUND_BACK);
        Hide();
    } else {
        UiSoundPlay(UI_SOUND_ERROR); // Nothing to go back to yet
    }
}

void DashboardScene::Launch(const Game &game)
{
    // Load the disc and restart the console: the boot animation, then the
    // game, as when a disc goes into a console.
    ActionLoadDiscFile(game.path.c_str());
    GameStart(KeyOf(game)); // Its own settings, and its play time
    std::string overrides;
    const GameProfile &profile = ProfileOf(game);
    for (int o = 0; o < GO__COUNT; o++) {
        if (profile.value[o] != GAME_DEFAULT) {
            overrides += std::string(overrides.empty() ? "" : ", ") +
                         GameOptionName(o) + " " +
                         GameOptionValueName(o, profile.value[o]);
        }
    }
    const XisoInfo &xiso = Art(game.path).info;
    std::vector<std::string> applied;
    std::string patches = PatchesStart(game.path, xiso.xbe_offset,
                                       xiso.xbe_size, game.title_id,
                                       KeyOf(game), &applied);
    WatchLaunch(game, applied, patches);
    GameLogStart({ game.name, game.path, KeyOf(game), xiso.title,
                   game.title_id, game.full_disc, overrides, patches });
    UiSoundPlay(UI_SOUND_LAUNCH);
    ActionReset();
    AddRecent(game.path);
    m_can_return = true;
    m_game_started = true;
    m_paused_vm = true; // Hide starts it (it may never have run)
    Hide();
}

//
// Recently played
//

static const size_t kRecentMax = 8;

static std::string RecentFile()
{
    return std::string(xemu_settings_get_base_path()) + "recent.txt";
}

void DashboardScene::LoadRecent()
{
    m_recent_loaded = true;
    std::ifstream in(RecentFile());
    std::string line;
    while (std::getline(in, line) && m_recent.size() < kRecentMax) {
        if (!line.empty()) {
            m_recent.push_back(line);
        }
    }
}

void DashboardScene::AddRecent(const std::string &path)
{
    m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), path),
                   m_recent.end());
    m_recent.insert(m_recent.begin(), path);
    if (m_recent.size() > kRecentMax) {
        m_recent.resize(kRecentMax);
    }
    std::ofstream out(RecentFile(), std::ios::trunc);
    for (const auto &p : m_recent) {
        out << p << "\n";
    }
    m_shelf_sel = 0;
}

// The recent games that are still in the games folder.
std::vector<const DashboardScene::Game *> DashboardScene::RecentGames()
{
    std::vector<const Game *> games;
    for (const auto &path : m_recent) {
        for (const auto &game : m_games) {
            if (game.path == path) {
                games.push_back(&game);
                break;
            }
        }
    }
    return games;
}

//
// After a launch: did the Xbox read the patched bytes, and is the game
// still what runs (a crash sends the Xbox back to its dashboard)?
//

static struct {
    bool active = false;
    double started = 0, next_check = 0;
    uint32_t title_id = 0;
    std::string name, patches;
    bool want_patches = false, patch_told = false, exit_told = false;
} g_watch;

void DashboardScene::WatchLaunch(const Game &game,
                                 const std::vector<std::string> &applied,
                                 const std::string &log)
{
    g_watch.active = true;
    g_watch.started = ImGui::GetTime();
    g_watch.next_check = g_watch.started + 10;
    g_watch.title_id = game.title_id;
    g_watch.name = game.name;
    g_watch.want_patches = !applied.empty();
    g_watch.patch_told = g_watch.exit_told = false;
    g_watch.patches.clear();
    for (const auto &n : applied) {
        g_watch.patches += (g_watch.patches.empty() ? "" : ", ") + n;
    }
    if (!applied.empty()) {
        xemu_queue_notification(("Patches on: " + g_watch.patches).c_str());
    }
    // Patches that were on but can't apply: say which.
    for (size_t at = 0; (at = log.find(" - skipped: ", at)) !=
                        std::string::npos;
         at++) {
        size_t line = log.rfind(": ", at);
        size_t end = log.find('\n', at);
        std::string name = log.substr(line + 2, at - line - 2);
        std::string why = log.substr(at + 12, end - at - 12);
        size_t dash = why.find(" - ");
        xemu_queue_notification(
            ("Patch not supported for your copy, skipped: " + name + " (" +
             (dash == std::string::npos ? why : why.substr(0, dash)) + ")")
                .c_str());
    }
}

void DashboardScene::WatchStop()
{
    g_watch.active = false;
}

void DashboardScene::WatchTick()
{
    double now = ImGui::GetTime();
    if (!g_watch.active || now < g_watch.next_check || !runstate_is_running()) {
        return;
    }
    g_watch.next_check = now + 2;

    if (g_watch.want_patches && !g_watch.patch_told) {
        if (PatchesBytesApplied() > 0) {
            g_watch.patch_told = true;
            xemu_queue_notification(
                ("Patch applied: " + g_watch.patches).c_str());
            GameLogNote("Patches applied as the game loaded: " +
                        g_watch.patches);
        } else if (now - g_watch.started > 30) {
            g_watch.patch_told = true;
            xemu_queue_notification("Patches were on, but the game didn't "
                                    "load the patched part (see "
                                    "xemu-game.log)");
            GameLogNote("Patches on (" + g_watch.patches +
                        ") but no patched bytes were read in 30 s");
        }
    }

    // What runs now: the game, or the Xbox's own dashboard after a crash
    // (or a quit to it).
    struct xbe *xbe = xemu_get_xbe_info();
    if (!xbe || !xbe->cert || g_watch.exit_told || !g_watch.title_id) {
        return;
    }
    uint32_t running = xbe->cert->m_titleid;
    if (running != g_watch.title_id && running != 0xFFFE0000) {
        // The game started another of its programs: follow that one.
        GameLogNote(g_watch.name + " started its program " +
                    [&] {
                        char id[16];
                        snprintf(id, sizeof(id), "%08X", running);
                        return std::string(id);
                    }());
        g_watch.title_id = running;
        return;
    }
    if (running == 0xFFFE0000) { // The Xbox dashboard's title ID
        g_watch.exit_told = true;
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s stopped and the Xbox went back to its dashboard: it "
                 "crashed or quit%s. See xemu-game.log",
                 g_watch.name.c_str(),
                 g_watch.patches.empty() ? "" : " - patches were on");
        xemu_queue_notification(buf);
        GameLogNote(std::string(buf) +
                    (g_watch.patches.empty() ? "" :
                                               " (" + g_watch.patches + ")"));
    }
}

void DashboardScene::OpenXboxDashboard()
{
    // An Xbox with no disc starts its own dashboard from the hard disk. If
    // that's what's running already (no game since XPSemu started), just
    // carry on with it.
    const char *dvd = g_config.sys.files.dvd_path;
    if (m_game_started || (dvd && dvd[0])) {
        if (dvd && dvd[0]) {
            ActionEjectDisc();
        }
        GameStop();
        PatchesStop();
        WatchStop();
        GameLogStop("back to the Xbox dashboard");
        ActionReset();
        m_game_started = false;
    }
    UiSoundPlay(UI_SOUND_LAUNCH);
    m_can_return = true;
    m_paused_vm = true; // Hide starts it (it may never have run)
    Hide();
}

// The Settings page's rows. Most are game options (a game can have its
// own value); Menu sounds and Advanced aren't.
enum SettingRow {
    SR_SCALE,
    SR_ASPECT,
    SR_FIT,
    SR_FILTER,
    SR_VOLUME,
    SR_SOUNDS,
    SR_OVERLAY,
    SR_ADVANCED,
    SR__COUNT
};
static const int kSettingOptions[SR__COUNT] = {
    GO_SCALE,  GO_ASPECT, GO_FIT,     GO_FILTER,
    GO_VOLUME, -1,        GO_OVERLAY, -1,
};

void DashboardScene::ChangeSetting(int step)
{
    if (m_setting == SR_SOUNDS) {
        g_config.display.ui.menu_sounds = !g_config.display.ui.menu_sounds;
        xemu_settings_save();
        UiSoundPlay(UI_SOUND_CHANGE); // Heard only when turned on
        return;
    }
    if (m_setting < 0 || m_setting >= SR__COUNT ||
        kSettingOptions[m_setting] < 0) {
        return;
    }
    if (m_setting == SR_SCALE && GameAnyRunning()) {
        UiSoundPlay(UI_SOUND_ERROR); // Locked: see DrawSettings
        return;
    }
    int option = kSettingOptions[m_setting];
    UiSoundPlay(UI_SOUND_CHANGE);
    GameSetGlobal(option, GameOptionStep(option, GameGlobal(option), step,
                                         false));
}

void DashboardScene::OpenGameSettings(const Game &game, bool from_shelf)
{
    m_gs_open = true;
    m_gs_from_shelf = from_shelf;
    m_gs_game = game;
    m_gs_key = KeyOf(game);
    m_gs_profile = GameProfileLoad(m_gs_key);
    m_gs_row = 0;
    m_gs_patches = false;
    m_patch_row = 0;
    m_gs_choices = PatchChoicesLoad(m_gs_key);
    m_gs_patch_items.clear();
    const XisoInfo &xiso = Art(game.path).info;
    for (const PatchFile *f : PatchesFor(game.title_id)) {
        for (int g = 0; g < (int)f->groups.size(); g++) {
            m_gs_patch_items.push_back(
                { f, g,
                  PatchGroupProblem(*f, f->groups[g], game.path,
                                    xiso.xbe_offset, xiso.xbe_size),
                  PatchGroupFoundInCopy(*f, f->groups[g], game.path,
                                        xiso.xbe_offset, xiso.xbe_size) });
        }
    }
    auto rank = [](const PatchItem &i) {
        return i.problem.empty() ? 0 : i.problem == "Info" ? 1 : 2;
    };
    std::stable_sort(m_gs_patch_items.begin(), m_gs_patch_items.end(),
                     [&](const PatchItem &a, const PatchItem &b) {
                         return rank(a) < rank(b);
                     });
    m_gs_crc_ok = PatchXbeCrc(game.path, xiso.xbe_offset, xiso.xbe_size,
                              &m_gs_crc);
    m_in_page = true;
}

// Rows: the options, then "Reset to defaults".
void DashboardScene::ChangeGameSetting(int step)
{
    if (m_gs_row == GS_PATCHES) {
        return; // Opens the list (HandleInputInner)
    }
    bool running = GameIsRunning(m_gs_key);
    if (m_gs_row == GO_SCALE && running) {
        UiSoundPlay(UI_SOUND_ERROR); // Locked while it runs
        return;
    }
    UiSoundPlay(m_gs_row == GS_RESET ? UI_SOUND_SELECT : UI_SOUND_CHANGE);
    if (m_gs_row == GS_RESET) {
        // A running game keeps its resolution (it can't change now).
        int scale = m_gs_profile.value[GO_SCALE];
        m_gs_profile = GameProfile();
        if (running) {
            m_gs_profile.value[GO_SCALE] = scale;
        }
    } else {
        int &v = m_gs_profile.value[m_gs_row];
        v = GameOptionStep(m_gs_row, v, step, true);
    }
    GameProfileSave(m_gs_key, m_gs_profile);
    g_profiles[m_gs_key] = m_gs_profile;
    if (GameIsRunning(m_gs_key)) {
        GameProfileReapply();
    }
}

//
// Advanced settings. Some only take effect when XPSemu starts (the Xbox is
// put together then); those are marked, and the page says so once changed.
//

enum AdvancedRow {
    ADV_XBOX_WIDESCREEN, // The Xbox's video settings (its EEPROM)
    ADV_XBOX_480P,
    ADV_XBOX_720P,
    ADV_XBOX_1080I,
    ADV_MEMORY,
    ADV_AVPACK,
    ADV_BOOT_ANIM,
    ADV_DSP,
    ADV_FPU,
    ADV_SHADER_CACHE,
    ADV_NOTIFICATIONS,
    ADV_PINNING,
    ADV_PROFILER,
    ADV__COUNT
};

// Video outputs in the order they're offered: the common ones first.
static const int kAvpacks[] = {
    CONFIG_SYS_AVPACK_HDTV,   CONFIG_SYS_AVPACK_COMPOSITE,
    CONFIG_SYS_AVPACK_SVIDEO, CONFIG_SYS_AVPACK_SCART,
    CONFIG_SYS_AVPACK_VGA,    CONFIG_SYS_AVPACK_RFU,
};
static const int kAvpackCount = sizeof(kAvpacks) / sizeof(kAvpacks[0]);

static const char *AvpackName(int avpack)
{
    switch (avpack) {
    case CONFIG_SYS_AVPACK_HDTV: return "HDTV (component)";
    case CONFIG_SYS_AVPACK_COMPOSITE: return "Composite";
    case CONFIG_SYS_AVPACK_SVIDEO: return "S-Video";
    case CONFIG_SYS_AVPACK_SCART: return "SCART";
    case CONFIG_SYS_AVPACK_VGA: return "VGA";
    case CONFIG_SYS_AVPACK_RFU: return "RF";
    default: return "None";
    }
}

// Shown but fixed: XPSemu runs the Xbox with 64 MB (xemu.c).
static bool AdvancedLocked(int row)
{
#ifdef __PROSPERO__
    return row == ADV_MEMORY;
#else
    (void)row;
    return false;
#endif
}

static bool AdvancedNeedsRestart(int row)
{
    return row == ADV_MEMORY || row == ADV_PINNING || row == ADV_AVPACK || row == ADV_BOOT_ANIM ||
           row == ADV_FPU || row == ADV_SHADER_CACHE;
}

// The video flag of an Advanced row (0 if it isn't one).
static uint32_t AdvancedVideoFlag(int row)
{
    switch (row) {
    case ADV_XBOX_WIDESCREEN: return XBOX_VIDEO_WIDESCREEN;
    case ADV_XBOX_480P: return XBOX_VIDEO_480P;
    case ADV_XBOX_720P: return XBOX_VIDEO_720P;
    case ADV_XBOX_1080I: return XBOX_VIDEO_1080I;
    }
    return 0;
}

void DashboardScene::ChangeAdvanced(int step)
{
    if (uint32_t flag = AdvancedVideoFlag(m_adv_setting)) {
        uint32_t flags;
        if (!xemu_eeprom_get_video_flags(&flags)) {
            UiSoundPlay(UI_SOUND_ERROR);
            xemu_queue_notification("Can't read the Xbox's settings (EEPROM)");
            return;
        }
        flags ^= flag;
        if (flag == XBOX_VIDEO_WIDESCREEN && (flags & flag)) {
            flags &= ~XBOX_VIDEO_LETTERBOX; // One or the other
        }
        UiSoundPlay(UI_SOUND_CHANGE);
        if (!xemu_eeprom_set_video_flags(flags)) {
            xemu_queue_notification("Couldn't save the Xbox's settings");
        } else if (GameAnyRunning()) {
            xemu_queue_notification("Xbox video settings: apply when a game "
                                    "starts");
        }
        return;
    }
    switch (m_adv_setting) {
    case ADV_MEMORY:
        if (AdvancedLocked(ADV_MEMORY)) {
            UiSoundPlay(UI_SOUND_ERROR);
            return;
        }
        g_config.sys.mem_limit =
            g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128 ?
                CONFIG_SYS_MEM_LIMIT_64 :
                CONFIG_SYS_MEM_LIMIT_128;
        break;
    case ADV_AVPACK: {
        int i = 0;
        while (i < kAvpackCount - 1 && kAvpacks[i] != g_config.sys.avpack) {
            i++;
        }
        g_config.sys.avpack = kAvpacks[(i + kAvpackCount + step) % kAvpackCount];
        break;
    }
    case ADV_BOOT_ANIM:
        g_config.general.skip_boot_anim = !g_config.general.skip_boot_anim;
        break;
    case ADV_DSP:
        GameSetGlobal(GO_DSP, !GameGlobal(GO_DSP));
        return; // Saved

    case ADV_FPU:
        g_config.perf.hard_fpu = !g_config.perf.hard_fpu;
        break;
    case ADV_SHADER_CACHE:
        g_config.perf.cache_shaders = !g_config.perf.cache_shaders;
        break;
    case ADV_NOTIFICATIONS:
        g_config.display.ui.show_notifications =
            !g_config.display.ui.show_notifications;
        break;
    case ADV_PINNING:
        g_config.perf.cpu_pinning = !g_config.perf.cpu_pinning;
        break;
    case ADV_PROFILER:
        g_config.perf.profiler = !g_config.perf.profiler;
        break;
    default:
        return;
    }
    if (AdvancedNeedsRestart(m_adv_setting)) {
        m_restart_needed = true;
    }
    xemu_settings_save();
}

// How deep the focus is, and where: for the navigation sounds.
struct FocusState {
    int depth;
    int where[9];
    bool operator==(const FocusState &o) const
    {
        return depth == o.depth && !memcmp(where, o.where, sizeof(where));
    }
};

void DashboardScene::HandleInput()
{
    auto state = [this]() {
        return FocusState{ m_in_page + m_advanced + m_gs_open + m_gs_patches,
                           { m_page, m_game, m_setting, m_adv_setting,
                             m_gs_row + 100 * m_patch_row, m_system_row,
                             m_shelf, m_shelf_sel,
                             m_visible } };
    };
    FocusState before = state();
    g_ui_sound_played = false;
    HandleInputInner();
    FocusState after = state();
    if (g_ui_sound_played || !m_visible) {
        return; // Launched, closed, or a value changed: it made its sound
    }
    if (after.depth > before.depth) {
        UiSoundPlay(UI_SOUND_SELECT);
    } else if (after.depth < before.depth) {
        UiSoundPlay(UI_SOUND_BACK);
    } else if (!(after == before)) {
        UiSoundPlay(UI_SOUND_MOVE);
    }
}

void DashboardScene::HandleInputInner()
{
    uint32_t buttons = g_input_mgr.CombinedButtons();
    uint32_t pressed = buttons & ~m_prev_buttons;
    m_prev_buttons = buttons;

    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    bool left = key(ImGuiKey_GamepadDpadLeft) ||
                key(ImGuiKey_GamepadLStickLeft);
    bool right = key(ImGuiKey_GamepadDpadRight) ||
                 key(ImGuiKey_GamepadLStickRight);
    bool up = key(ImGuiKey_GamepadDpadUp) || key(ImGuiKey_GamepadLStickUp);
    bool down = key(ImGuiKey_GamepadDpadDown) ||
                key(ImGuiKey_GamepadLStickDown);
    bool accept = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false);
    bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
    bool l1 = key(ImGuiKey_GamepadL1), r1 = key(ImGuiKey_GamepadR1);
    bool options = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);

    // The touchpad (Guide) closes the dashboard, back to the game.
    if (pressed & CONTROLLER_BUTTON_GUIDE) {
        Close();
        return;
    }

    if (!m_in_page && m_shelf) { // The recently played shelf
        auto recent = RecentGames();
        int n = recent.size();
        if (!n || up) {
            m_shelf = false;
            return;
        }
        m_shelf_sel = std::clamp(m_shelf_sel, 0, n - 1);
        if (left) m_shelf_sel = std::max(m_shelf_sel - 1, 0);
        if (right) m_shelf_sel = std::min(m_shelf_sel + 1, n - 1);
        if (accept) Launch(*recent[m_shelf_sel]);
        else if (options) OpenGameSettings(*recent[m_shelf_sel], true);
        else if (back) Close();
        return;
    }

    if (!m_in_page) { // The main menu
        bool has_shelf = !RecentGames().empty();
        if (up) m_page = (m_page + MENU__COUNT - 1) % MENU__COUNT;
        if (down) {
            if (m_page == MENU__COUNT - 1 && has_shelf) {
                m_shelf = true; // Down from the last item: the shelf
                return;
            }
            m_page = (m_page + 1) % MENU__COUNT;
        }
        if (accept || right) {
            if (m_page == MENU_XBOX_DASHBOARD) {
                OpenXboxDashboard();
            } else {
                m_in_page = true;
            }
        }
        if (back) Close();
        return;
    }

    if (m_gs_open && m_gs_patches) { // A game's patches
        int n = m_gs_patch_items.size();
        if (back) {
            m_gs_patches = false;
            return;
        }
        if (!n) {
            return;
        }
        if (up) m_patch_row = (m_patch_row + n - 1) % n;
        if (down) m_patch_row = (m_patch_row + 1) % n;
        if (left || right || accept) {
            const auto &item = m_gs_patch_items[m_patch_row];
            const PatchGroup &g = item.file->groups[item.group];
            if (!item.problem.empty()) {
                UiSoundPlay(UI_SOUND_ERROR);
                if (item.problem != "Info") {
                    xemu_queue_notification(
                        (g.name + ": not supported for your copy (" +
                         item.problem + ")")
                            .c_str());
                }
                return;
            }
            bool on = !PatchGroupOn(m_gs_choices, *item.file, g);
            m_gs_choices[PatchGroupId(*item.file, g)] = on;
            PatchChoicesSave(m_gs_key, m_gs_choices);
            UiSoundPlay(UI_SOUND_CHANGE);
            std::string note = g.name + (on ? ": on" : ": off") + " - " +
                               (GameIsRunning(m_gs_key) ?
                                    "applies when the game restarts" :
                                    "applies when " + m_gs_game.name +
                                        " starts");
            // A widescreen patch draws wider; the TV picture must be too.
            std::string lower = g.name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           ::tolower);
            if (on && lower.find("widescreen") != std::string::npos &&
                m_gs_profile.value[GO_ASPECT] == GAME_DEFAULT &&
                GameGlobal(GO_ASPECT) != CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9) {
                m_gs_profile.value[GO_ASPECT] =
                    CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9;
                GameProfileSave(m_gs_key, m_gs_profile);
                g_profiles[m_gs_key] = m_gs_profile;
                note += ". Screen shape set to 16:9 for this game";
            }
            xemu_queue_notification(note.c_str());
        }
        return;
    }

    if (m_gs_open) { // A game's settings
        const int rows = GS__COUNT;
        if (back) {
            m_gs_open = false;
            if (m_gs_from_shelf) {
                m_in_page = false;
            }
            return;
        }
        if (up) m_gs_row = (m_gs_row + rows - 1) % rows;
        if (down) m_gs_row = (m_gs_row + 1) % rows;
        if (m_gs_row == GS_PATCHES && (accept || right)) {
            m_gs_patches = true;
            m_patch_row = 0;
            return;
        }
        if (m_gs_row == GS_RESET ? accept : (left || right || accept)) {
            ChangeGameSetting(left ? -1 : 1);
        }
        return;
    }

    if (back) {
        if (m_page == PAGE_SETTINGS && m_advanced) {
            m_advanced = false;
        } else {
            m_in_page = false;
        }
        return;
    }

    switch (m_page) {
    case PAGE_GAMES: {
        int n = m_games.size();
        if (!n) {
            break;
        }
        if (left || up) m_game = std::max(m_game - 1, 0);
        if (right || down) m_game = std::min(m_game + 1, n - 1);
        if (l1) m_game = std::max(m_game - 5, 0);
        if (r1) m_game = std::min(m_game + 5, n - 1);
        if (accept) Launch(m_games[m_game]);
        else if (options) OpenGameSettings(m_games[m_game], false);
        break;
    }
    case PAGE_SETTINGS: {
        if (m_advanced) {
            const int rows = ADV__COUNT;
            if (up) m_adv_setting = (m_adv_setting + rows - 1) % rows;
            if (down) m_adv_setting = (m_adv_setting + 1) % rows;
            if (left || right || accept) {
                ChangeAdvanced(left ? -1 : 1);
            }
            break;
        }
        const int rows = SR__COUNT; // The last is Advanced
        if (up) m_setting = (m_setting + rows - 1) % rows;
        if (down) m_setting = (m_setting + 1) % rows;
        if (m_setting == rows - 1) { // Advanced
            if (accept || right) {
                m_advanced = true;
            }
        } else if (left || right || accept) {
            ChangeSetting(left ? -1 : 1);
        }
        break;
    }
    case PAGE_SYSTEM:
        if (up || down) m_system_row ^= 1;
        if (accept && m_system_row == 0) { // Restart the console
            ActionReset();
            m_can_return = true;
            m_paused_vm = true; // Hide starts it (it may never have run)
            Hide();
        }
        if (accept && m_system_row == 1) { // Eject the disc
            ActionEjectDisc();
        }
        break;
    }
}

bool DashboardScene::Draw()
{
    ImGuiIO &io = ImGui::GetIO();
    float dt = io.DeltaTime;
    m_time += dt;
    m_alpha = Approach(m_alpha, m_visible ? 1 : 0, dt, 8);
    if (!m_visible && m_alpha < 0.01f) {
        m_alpha = 0;
        return false;
    }

    // A window that takes focus, so the controller drives the dashboard and
    // not the game (see InputManager::Update).
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::Begin("###Dashboard", NULL,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar();

    float s = io.DisplaySize.y / 1080.0f; // Layout is in 1080p units
    g_texture_budget = 2;
    if (m_visible) {
        HandleInput();
    }
    m_page_anim = Approach(m_page_anim, m_in_page ? 1 : 0, dt, 10);
    m_menu_anim = Approach(m_menu_anim, m_page, dt, 16);
    m_game_anim = Approach(m_game_anim, m_game, dt, 14);
    m_shelf_anim = Approach(m_shelf_anim, m_shelf_sel, dt, 16);
    m_shelf_focus = Approach(m_shelf_focus, m_shelf && !m_in_page ? 1 : 0, dt,
                             10);

    DrawBackground(s);
    // The main menu slides out to the left as a page slides in.
    if (m_page_anim < 0.99f) {
        DrawMainMenu(s, m_alpha * (1 - m_page_anim));
    }
    if (m_page_anim > 0.01f) {
        float a = m_alpha * m_page_anim;
        DrawPageHeader(s, a);
        if (m_gs_open) {
            DrawGameSettings(s, a);
        } else switch (m_page) {
        case PAGE_GAMES: DrawGames(s, a); break;
        case PAGE_SETTINGS:
            if (m_advanced) {
                DrawAdvanced(s, a);
            } else {
                DrawSettings(s, a);
            }
            break;
        case PAGE_SYSTEM: DrawSystem(s, a); break;
        }
    }
    DrawHints(s);

    ImGui::End();
    return true;
}

//
// Drawing
//

void DashboardScene::DrawBackground(float s)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float a = m_alpha;

    // Dark green, a little lighter in the middle.
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, Rgba(4, 40, 4, a),
                                Rgba(2, 26, 2, a), Rgba(0, 12, 0, a),
                                Rgba(2, 24, 2, a));

    // A fine grid bowed like the inside of a sphere, drifting slowly.
    ImU32 grid = Rgba(40, 110, 20, a * 0.35f);
    float drift = fmodf(m_time * 6 * s, 80 * s);
    for (float x = -80 * s + drift; x < size.x + 80 * s; x += 80 * s) {
        float bow = (x - size.x / 2) / size.x * 60 * s;
        dl->AddBezierQuadratic(ImVec2(x - bow, 0), ImVec2(x + bow, size.y / 2),
                               ImVec2(x - bow, size.y), grid, 1.2f * s, 12);
    }
    for (float y = -80 * s + drift; y < size.y + 80 * s; y += 80 * s) {
        float bow = (y - size.y / 2) / size.y * 60 * s;
        dl->AddBezierQuadratic(ImVec2(0, y - bow), ImVec2(size.x / 2, y + bow),
                               ImVec2(size.x, y - bow), grid, 1.2f * s, 12);
    }

    // A few motes of light drifting up.
    static struct { float x, y, speed, phase; } motes[40];
    static bool seeded;
    if (!seeded) {
        for (auto &m : motes) {
            m.x = (rand() % 10000) / 10000.0f;
            m.y = (rand() % 10000) / 10000.0f;
            m.speed = 0.01f + (rand() % 1000) / 1000.0f * 0.025f;
            m.phase = (rand() % 1000) / 1000.0f * 6.28f;
        }
        seeded = true;
    }
    float dt = ImGui::GetIO().DeltaTime;
    for (auto &m : motes) {
        m.y -= m.speed * dt;
        if (m.y < -0.02f) {
            m.y = 1.02f;
        }
        float twinkle = 0.5f + 0.5f * sinf(m_time * 2 + m.phase);
        dl->AddCircleFilled(ImVec2(size.x * m.x, size.y * m.y),
                            (1.5f + twinkle) * s, Lime(a * 0.5f * twinkle), 8);
    }
}

// A glowing yellow-green orb with a slowly turning ring of light around it.
void DashboardScene::DrawOrb(float cx, float cy, float r)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float a = m_alpha;
    ImVec2 c(cx, cy);
    float breathe = 1 + 0.02f * sinf(m_time * 2);

    // Halo, then the sphere from its rim to its bright core.
    dl->AddCircleFilled(c, r * 1.35f, Rgba(90, 200, 30, a * 0.10f), 64);
    dl->AddCircleFilled(c, r * 1.15f, Rgba(90, 200, 30, a * 0.14f), 64);
    dl->AddCircleFilled(c, r * breathe, Rgba(40, 140, 15, a), 64);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.08f, c.y - r * 0.08f), r * 0.82f,
                        Rgba(110, 200, 25, a), 64);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.15f), r * 0.6f,
                        Rgba(175, 230, 35, a), 64);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y - r * 0.2f), r * 0.38f,
                        Rgba(225, 245, 70, a), 48);
    // Shine.
    dl->AddCircleFilled(ImVec2(c.x - r * 0.42f, c.y - r * 0.45f), r * 0.1f,
                        White(a * 0.8f), 24);

    // Two tilted rings turning around it, like the original's ribbons.
    for (int ring = 0; ring < 2; ring++) {
        float spin = m_time * (ring ? -0.5f : 0.35f) + ring * 1.2f;
        float tilt = ring ? 0.35f : 0.22f;
        const int n = 64;
        for (int i = 0; i < n; i++) {
            float t0 = spin + 2 * M_PI * i / n, t1 = spin + 2 * M_PI * (i + 1) / n;
            float z = sinf((t0 + t1) / 2); // Front of the ring is brighter
            ImVec2 p0(c.x + cosf(t0) * r * 1.22f,
                      c.y + sinf(t0) * r * 1.22f * tilt + cosf(t0) * r * 0.25f);
            ImVec2 p1(c.x + cosf(t1) * r * 1.22f,
                      c.y + sinf(t1) * r * 1.22f * tilt + cosf(t1) * r * 0.25f);
            dl->AddLine(p0, p1, Lime(a * (0.25f + 0.6f * (z + 1) / 2)),
                        r * 0.03f * (1 + (z + 1) / 2));
        }
    }
}

void DashboardScene::DrawMainMenu(float s, float a)
{
    static const char *const names[MENU__COUNT] = { "Games", "Settings",
                                                    "System",
                                                    "Xbox Dashboard" };
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = -300 * s * m_page_anim;
    float save_alpha = m_alpha;
    m_alpha = a;

    // With games played, the menu moves up to make room for the shelf.
    auto recent = RecentGames();
    bool shelf = !recent.empty();
    float cy = (shelf ? 410 : 540) * s;

    // The orb, or on the shelf the focused game's cover floating in its
    // place.
    float k = shelf ? m_shelf_focus : 0;
    if (k < 0.99f) {
        m_alpha = a * (1 - k);
        DrawOrb(470 * s + slide, cy, (shelf ? 210 : 250) * s);
        m_alpha = a;
    }
    if (k > 0.01f) {
        const Game &game =
            *recent[std::clamp(m_shelf_sel, 0, (int)recent.size() - 1)];
        DrawCoverFloat(game, ImVec2(470 * s + slide, cy), 460 * s, a * k);
    }

    // The name, in the orb's glow.
    DrawTitle(s, a, ImVec2(90 * s + slide, 70 * s));
    Text(dl, small, 28 * s, ImVec2(96 * s + slide, 150 * s), Label(a * 0.7f),
         "Xbox");

    float x = 900 * s - slide, w = 820 * s, h = (shelf ? 88 : 96) * s;
    float gap = (shelf ? 118 : 140) * s;
    float y0 = cy - gap * (MENU__COUNT - 1) / 2 - h / 2;
    float ma = a * (1 - 0.35f * k); // The menu steps back for the shelf
    for (int i = 0; i < MENU__COUNT; i++) {
        float y = y0 + i * gap;
        bool selected = i == m_page && !m_shelf;
        // The socket in front of each bar.
        ImVec2 sc(x - 70 * s, y + h / 2);
        dl->AddCircle(sc, 40 * s, selected ? Lime(ma) : Line(ma), 40, 4 * s);
        dl->AddCircleFilled(sc, 28 * s,
                            selected ? Rgba(120, 210, 30, ma) :
                                       Rgba(8, 36, 6, ma),
                            40);
        dl->AddCircle(sc, 11 * s, selected ? Ink(ma) : Line(ma), 24, 3 * s);
        dl->AddLine(ImVec2(sc.x + 40 * s, sc.y), ImVec2(x, sc.y), Line(ma),
                    2 * s);

        ImVec2 p0(x, y), p1(x + w, y + h);
        Bar(dl, p0, p1, s, selected, ma);
        std::string label = Upper(names[i]);
        ImVec2 ts = TextSize(font, 58 * s, label.c_str());
        Text(dl, font, 58 * s, ImVec2(x + 50 * s, y + (h - ts.y) / 2),
             selected ? Ink(ma) : Label(ma), label.c_str());
    }
    // The focus cue follows the selection smoothly.
    if (k < 0.5f) {
        float fy = y0 + m_menu_anim * gap;
        Focus(dl, ImVec2(x, fy), ImVec2(x + w, fy + h), s, m_time,
              a * (1 - 2 * k));
    }

    if (shelf) {
        DrawShelf(s, a, 150 * s + slide, 680 * s);
    }
    m_alpha = save_alpha;
}

// A game's cover standing in for the orb: glowing, bobbing gently, with a
// ring of light turning behind it.
void DashboardScene::DrawCoverFloat(const Game &game, ImVec2 c, float h,
                                    float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float s = ImGui::GetIO().DisplaySize.y / 1080.0f;
    c.y += sinf(m_time * 1.6f) * 8 * s;

    dl->AddCircleFilled(c, h * 0.62f, Rgba(90, 200, 30, a * 0.10f), 64);
    dl->AddCircleFilled(c, h * 0.5f, Rgba(90, 200, 30, a * 0.10f), 64);
    float spin = m_time * 0.6f;
    dl->PathArcTo(c, h * 0.56f, spin, spin + 2.2f, 40);
    dl->PathStroke(Lime(a * 0.7f), 0, 4 * s);
    dl->PathArcTo(c, h * 0.56f, spin + M_PI, spin + M_PI + 1.2f, 24);
    dl->PathStroke(Lime(a * 0.4f), 0, 4 * s);

    float w = h * 0.72f; // Xbox game boxes are about this shape
    DrawGameArt(game, ImVec2(c.x - w / 2, c.y - h / 2),
                ImVec2(c.x + w / 2, c.y + h / 2), a, true);
}

// The recently played shelf: covers in a row with a faint reflection, the
// focused one lifted, and beside them the focused game's name and play time.
void DashboardScene::DrawShelf(float s, float a, float x0, float y0)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    auto recent = RecentGames();
    int n = recent.size();
    int sel = std::clamp(m_shelf_sel, 0, n - 1);
    float f = m_shelf ? m_shelf_focus : 0;

    Text(dl, small, 30 * s, ImVec2(x0, y0), Lime(a), "RECENTLY PLAYED");
    dl->AddLine(ImVec2(x0, y0 + 44 * s), ImVec2(x0 + 1600 * s, y0 + 44 * s),
                Line(a * 0.35f), 1.5f * s);

    // Room above the covers for the focused one to lift and its outline.
    float bw = 116 * s, bh = 162 * s, gap = 36 * s, by = y0 + 78 * s;
    float lift_max = 12 * s, grow_max = 0.1f;
    for (int i = 0; i < n; i++) {
        bool focused = m_shelf && i == sel;
        float lift = focused ? lift_max * f : 0;
        float grow = focused ? 1 + grow_max * f : 1;
        float cx = x0 + i * (bw + gap) + bw / 2;
        float w = bw * grow, h = bh * grow;
        ImVec2 p0(cx - w / 2, by + bh - h - lift);
        ImVec2 p1(cx + w / 2, by + bh - lift);
        float ia = a * (m_shelf && !focused ? 0.6f : 1);
        dl->AddRectFilled(p0, p1, Rgba(10, 40, 6, ia * 0.8f), 8 * s);
        DrawGameArt(*recent[i], p0, p1, ia, true);

        // The reflection of a cover: its bottom, flipped and fading.
        XemuTexture tex = Art(recent[i]->path).cover;
        if (tex) {
            float rh = 30 * s;
            ImVec2 r0(p0.x, by + bh + 6 * s), r1(p1.x, by + bh + 6 * s + rh);
            dl->AddImage((ImTextureID)tex, r0, r1, ImVec2(0, 1),
                         ImVec2(1, 1 - rh / h), White(ia * 0.18f));
            dl->AddRectFilledMultiColor(r0, r1, Rgba(2, 20, 2, 0),
                                        Rgba(2, 20, 2, 0), Rgba(2, 20, 2, ia),
                                        Rgba(2, 20, 2, ia));
        }
    }
    if (m_shelf) {
        float fx = x0 + m_shelf_anim * (bw + gap);
        float g = grow_max * f, lift = lift_max * f;
        Focus(dl, ImVec2(fx - bw * g / 2, by - bh * g - lift),
              ImVec2(fx + bw * (1 + g / 2), by + bh - lift), s, m_time, a * f);
    }

    // Beside the covers: the focused game (the newest when the menu has
    // the focus), its play time and when it was last played.
    const Game &game = *recent[m_shelf ? sel : 0];
    float px = x0 + std::max(n, 3) * (bw + gap) + 40 * s;
    float pw = std::max(300 * s, 1860 * s - px);
    dl->AddLine(ImVec2(px - 22 * s, by + 8 * s),
                ImVec2(px - 22 * s, by + bh - 8 * s), Line(a * 0.8f), 2 * s);
    Text(dl, font, 44 * s, ImVec2(px, by + 6 * s), White(a),
         game.name.c_str(), pw);
    ImVec2 ns = font->CalcTextSizeA(44 * s, FLT_MAX, pw, game.name.c_str());
    GamePlay play = GamePlayGet(KeyOf(game));
    std::string time_text = GamePlayTimeText(play.seconds);
    std::string last_text = "Last played  " + GamePlayLastText(play.last);
    float ly = by + 16 * s + ns.y;
    // A small clock before the play time.
    ImVec2 cc(px + 14 * s, ly + 18 * s);
    dl->AddCircle(cc, 12 * s, Lime(a), 20, 2.5f * s);
    dl->AddLine(cc, ImVec2(cc.x, cc.y - 7 * s), Lime(a), 2.5f * s);
    dl->AddLine(cc, ImVec2(cc.x + 6 * s, cc.y), Lime(a), 2.5f * s);
    Text(dl, small, 32 * s, ImVec2(px + 40 * s, ly), Lime(a),
         time_text.c_str());
    Text(dl, small, 28 * s, ImVec2(px, ly + 44 * s), Label(a * 0.8f),
         last_text.c_str());
}

// "XPSemu", with a glint of light sweeping across it now and then.
void DashboardScene::DrawTitle(float s, float a, ImVec2 pos)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font;
    const char *title = "XPSemu";
    float size = 70 * s;
    ImVec2 ts = TextSize(font, size, title);

    // A sweep every 8 s, the first one soon after the dashboard opens.
    const float period = 8, duration = 1.2f;
    float phase = fmodf(m_time + period - 1.5f, period);
    float t = phase < duration ? phase / duration : -1;
    float glow = t >= 0 ? sinf(t * M_PI) : 0; // 0 .. 1 .. 0

    // A soft green halo behind the letters while it shines.
    if (glow > 0) {
        for (int i = 0; i < 8; i++) {
            float ang = i * M_PI / 4, r = 3 * s;
            Text(dl, font, size,
                 ImVec2(pos.x + cosf(ang) * r, pos.y + sinf(ang) * r),
                 Lime(a * 0.10f * glow), title);
        }
    }
    // The letters, a little brighter while it shines.
    Text(dl, font, size, pos,
         Rgba(150 + (int)(70 * glow), 235 + (int)(20 * glow),
              60 + (int)(90 * glow), a),
         title);
    if (t < 0) {
        return;
    }

    // The glint: the letters again in white, each vertex only as opaque
    // as it is close to a slanted band moving left to right.
    int v0 = dl->VtxBuffer.Size;
    Text(dl, font, size, pos, White(a), title);
    int v1 = dl->VtxBuffer.Size;
    float slant = 0.45f, width = size * 0.55f;
    float band = pos.x - ts.y * slant - width +
                 t * (ts.x + ts.y * slant + 2 * width);
    for (int i = v0; i < v1; i++) {
        ImDrawVert &v = dl->VtxBuffer[i];
        float d = v.pos.x + (v.pos.y - pos.y) * slant - band;
        float k = std::max(0.f, 1 - fabsf(d) / width);
        int alpha = (int)(((v.col >> IM_COL32_A_SHIFT) & 0xff) * k * k);
        v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)alpha << IM_COL32_A_SHIFT);
    }

    // As it leaves, a sparkle at the end of the word.
    if (t > 0.7f) {
        float k = sinf((t - 0.7f) / 0.3f * M_PI);
        ImVec2 c(pos.x + ts.x + 6 * s, pos.y + size * 0.2f);
        float r = 16 * s * k;
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), White(a * k),
                    2.5f * s);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), White(a * k),
                    2.5f * s);
        dl->AddCircleFilled(c, 3.5f * s * k, White(a * k), 12);
    }
}

// A game's own settings: each "Default" (the Settings page's value) or its
// own; saved per game and applied whenever it starts. Then its patches, and
// Reset. With the patches open, the list of them instead.
void DashboardScene::DrawGameSettings(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);
    float x = 280 * s + slide, w = 1060 * s;

    if (m_gs_patches) {
        DrawGamePatches(s, a, x, w);
    } else {
        float h = 60 * s, gap = 72 * s, y0 = 226 * s;
        for (int i = 0; i < GS__COUNT; i++) {
            float y = y0 + i * gap;
            bool selected = i == m_gs_row;
            Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, a);
            const char *name = i < GO__COUNT   ? GameOptionName(i) :
                               i == GS_PATCHES ? "Game patches" :
                                                 "Reset to defaults";
            Text(dl, font, 40 * s, ImVec2(x + 40 * s, y + 9 * s),
                 selected ? Ink(a) : Label(a), name);
            char value[64] = "";
            ImU32 col = selected ? Ink(a) : Lime(a);
            if (i == GS_PATCHES) {
                int on = 0, usable = 0;
                for (const auto &item : m_gs_patch_items) {
                    if (!item.problem.empty()) {
                        continue;
                    }
                    usable++;
                    on += PatchGroupOn(m_gs_choices, *item.file,
                                       item.file->groups[item.group]);
                }
                if (m_gs_patch_items.empty() || !usable) {
                    snprintf(value, sizeof(value),
                             selected ? "%s  >" : "%s",
                             m_gs_patch_items.empty() ? "None found" :
                                                        "None for your copy");
                    col = selected ? Ink(a) : White(a * 0.55f);
                } else {
                    snprintf(value, sizeof(value), selected ? "%d of %d on  >" :
                                                              "%d of %d on",
                             on, usable);
                }
            } else if (i < GO__COUNT) {
                bool locked = i == GO_SCALE && GameIsRunning(m_gs_key);
                int v = m_gs_profile.value[i];
                if (v == GAME_DEFAULT) {
                    std::string global = GameOptionValueLabel(i, GameGlobal(i));
                    snprintf(value, sizeof(value),
                             locked     ? "Default (%s)  (locked)" :
                             selected   ? "<  Default (%s)  >" :
                                          "Default (%s)",
                             global.c_str());
                    col = selected ? Ink(a) : White(a * 0.55f);
                } else {
                    snprintf(value, sizeof(value),
                             locked   ? "%s  (locked)" :
                             selected ? "<  %s  >" :
                                        "%s",
                             GameOptionValueLabel(i, v));
                    if (locked && !selected) {
                        col = White(a * 0.55f);
                    }
                }
            }
            if (value[0]) {
                ImVec2 vs = TextSize(font, 40 * s, value);
                Text(dl, font, 40 * s, ImVec2(x + w - vs.x - 50 * s, y + 9 * s),
                     col, value);
            }
        }
        Focus(dl, ImVec2(x, y0 + m_gs_row * gap),
              ImVec2(x + w, y0 + m_gs_row * gap + h), s, m_time, a);

        const char *help =
            m_gs_row == GO_SCALE && GameIsRunning(m_gs_key) ?
                "Locked while this game runs. Change it before starting it "
                "(or after the Xbox Dashboard button)." :
            m_gs_row < GO__COUNT ? GameOptionHelp(m_gs_row) :
            m_gs_row == GS_PATCHES ?
                "Widescreen, 60 FPS and other fixes from .JMP files in "
                "/data/xemu/patches, applied as the game loads: the disc "
                "image isn't changed." :
                "Back to Default for every setting: this game uses your "
                "Settings again (patches stay as they are).";
        Text(dl, small, 30 * s, ImVec2(x + 10 * s, y0 + GS__COUNT * gap + 8 * s),
             Label(a), help, w);
    }

    // The game: its art, name and play time.
    float cx = 1620 * s + slide;
    ImVec2 art0(cx - 150 * s, 230 * s), art1(cx + 150 * s, 650 * s);
    DrawGameArt(m_gs_game, art0, art1, a, true);
    const char *gname = m_gs_game.name.c_str();
    ImVec2 ns = font->CalcTextSizeA(40 * s, FLT_MAX, 440 * s, gname);
    float ty = art1.y + 26 * s;
    Text(dl, font, 40 * s, ImVec2(cx - ns.x / 2, ty), White(a), gname,
         440 * s);
    GamePlay play = GamePlayGet(m_gs_key);
    std::string played = "Played " + GamePlayTimeText(play.seconds);
    if (!play.last) {
        played = "Not played yet";
    }
    ImVec2 ps = TextSize(small, 30 * s, played.c_str());
    Text(dl, small, 30 * s, ImVec2(cx - ps.x / 2, ty + ns.y + 12 * s),
         Lime(a), played.c_str());
    if (GameIsRunning(m_gs_key)) {
        const char *now = m_gs_patches ?
                              "Running now: patches apply when it restarts" :
                              "Running now: resolution is locked, the "
                              "rest apply right away";
        ImVec2 rs = small->CalcTextSizeA(28 * s, FLT_MAX, 440 * s, now);
        Text(dl, small, 28 * s,
             ImVec2(cx - rs.x / 2, ty + ns.y + ps.y + 24 * s),
             Label(a * 0.8f), now, 440 * s);
    }
}

// The game's patches: every option of its .JMP files, on or off, or why it
// can't apply to this game's disc.
void DashboardScene::DrawGamePatches(float s, float a, float x, float w)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;

    if (m_gs_patch_items.empty()) {
        char id[16];
        snprintf(id, sizeof(id), "%08X", m_gs_game.title_id);
        Text(dl, font, 48 * s, ImVec2(x, 260 * s), White(a),
             "No patches for this game yet");
        std::string how =
            std::string("Put .JMP patch files (from the Xbox Patch Hub at "
                        "jayxbox.com) in /data/xemu/patches. This game's "
                        "title ID is ") +
            (m_gs_game.title_id ? id : "unknown") +
            ": a patch is for the game whose ID is on its version= line.";
        Text(dl, small, 32 * s, ImVec2(x, 340 * s), Label(a), how.c_str(), w);
        return;
    }

    const int visible = 7;
    float h = 60 * s, gap = 72 * s, y0 = 238 * s;
    int n = m_gs_patch_items.size();
    char crc_line[160];
    snprintf(crc_line, sizeof(crc_line),
             m_gs_crc_ok ? "Your copy: checksum %08X. Patches made for it "
                           "work; the rest are not supported." :
                           "Your copy's checksum couldn't be read.",
             m_gs_crc);
    Text(dl, small, 26 * s, ImVec2(x + 10 * s, y0 - 36 * s),
         Label(a * 0.8f), crc_line);
    int top = std::clamp(m_patch_row - visible / 2, 0, std::max(0, n - visible));
    for (int i = top; i < std::min(n, top + visible); i++) {
        const auto &item = m_gs_patch_items[i];
        const PatchGroup &g = item.file->groups[item.group];
        float y = y0 + (i - top) * gap;
        bool selected = i == m_patch_row;
        bool usable = item.problem.empty();
        float ra = usable ? a : a * 0.55f;
        Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, ra);

        bool on = usable && PatchGroupOn(m_gs_choices, *item.file, g);
        std::string value = !usable  ? (item.problem == "Info" ?
                                            "Info" :
                                            "Not supported") :
                            selected ? (on ? "<  On  >" : "<  Off  >") :
                            on       ? "On" :
                                       "Off";
        ImVec2 vs = TextSize(font, 40 * s, value.c_str());
        std::string name = g.name;
        ImVec2 ls = TextSize(font, 40 * s, name.c_str());
        while (ls.x > w - vs.x - 130 * s && name.size() > 4) {
            name.resize(name.size() - 4);
            name += "...";
            ls = TextSize(font, 40 * s, name.c_str());
        }
        Text(dl, font, 40 * s, ImVec2(x + 40 * s, y + 9 * s),
             selected ? Ink(a) : Label(ra), name.c_str());
        Text(dl, font, 40 * s, ImVec2(x + w - vs.x - 50 * s, y + 9 * s),
             selected ? Ink(a) : on ? Lime(a) : White(a * 0.55f),
             value.c_str());
    }
    Focus(dl, ImVec2(x, y0 + (m_patch_row - top) * gap),
          ImVec2(x + w, y0 + (m_patch_row - top) * gap + h), s, m_time, a);
    // More above or below: small arrows at the list's edge.
    if (top > 0) {
        Text(dl, small, 30 * s, ImVec2(x + w + 20 * s, y0 + 10 * s), Lime(a),
             "^");
    }
    if (top + visible < n) {
        Text(dl, small, 30 * s,
             ImVec2(x + w + 20 * s, y0 + (visible - 1) * gap + 20 * s),
             Lime(a), "v");
    }

    // The selection: its file, and what it says or why it can't apply.
    const auto &sel = m_gs_patch_items[m_patch_row];
    const PatchFile &f = *sel.file;
    std::string about = f.file_name;
    if (!f.author.empty()) {
        about += "  -  by " + f.author;
    }
    std::string detail =
        sel.problem == "Info"               ? f.notes :
        !f.unsupported.empty()              ?
            "Not supported: " + f.unsupported + "." :
        sel.problem == "Other version"      ?
            "Not supported: made for another release of this game (the "
            "file lists other checksums than your copy's)." :
        sel.problem == "Not in this game"   ?
            "Not supported: the code it changes isn't in your copy." :
        !sel.problem.empty()                ?
            "Not supported: " + sel.problem + "." :
        sel.found                           ?
            "Made for other releases, but the code it changes is in your "
            "copy (found once), so it works. " + f.notes :
                                              f.notes;
    float ty = y0 + visible * gap + 6 * s;
    ImVec2 as = small->CalcTextSizeA(30 * s, FLT_MAX, w, about.c_str());
    Text(dl, small, 30 * s, ImVec2(x + 10 * s, ty), Label(a), about.c_str(),
         w);
    if (!detail.empty()) {
        Text(dl, small, 28 * s, ImVec2(x + 10 * s, ty + as.y + 8 * s),
             sel.problem.empty() || sel.problem == "Info" ? White(a * 0.7f) :
                                                            Lime(a),
             detail.c_str(), w);
    }
}

void DashboardScene::DrawPageHeader(float s, float a)
{
    static const char *const names[PAGE__COUNT] = { "Games", "Settings",
                                                    "System" };
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font;
    float slide = 200 * s * (1 - m_page_anim);
    float save_alpha = m_alpha;
    m_alpha = a;

    DrawOrb(150 * s, 130 * s, 70 * s);
    ImVec2 p0(280 * s + slide, 80 * s), p1(1500 * s + slide, 180 * s);
    Bar(dl, p0, p1, s, true, a);
    // A game's settings opened on the shelf leave m_page on the menu's last
    // item, which isn't a page: as that page slides away after Back, keep
    // the title it had (names[] has only the pages).
    if (m_gs_open) {
        m_header = m_gs_patches ? "Game patches" : "Game settings";
    } else if (m_page == PAGE_SETTINGS && m_advanced) {
        m_header = "Advanced settings";
    } else if (m_page >= 0 && m_page < PAGE__COUNT) {
        m_header = names[m_page];
    }
    std::string title = Upper(m_header.c_str());
    Text(dl, font, 64 * s, ImVec2(p0.x + 50 * s, p0.y + 16 * s), Ink(a),
         title.c_str());
    m_alpha = save_alpha;
}

// A game case in any four-cornered shape (tl, tr, br, bl clockwise), so the
// covers either side of the carousel can be turned in perspective: the
// cover picture, else a plain Xbox case (green band, the disc's logo).
// Returns the cover texture drawn, if any, for the reflection.
static XemuTexture DrawCase(const DashboardScene::Game &game, ImVec2 tl,
                            ImVec2 tr, ImVec2 br, ImVec2 bl, float a,
                            float dim)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float s = ImGui::GetIO().DisplaySize.y / 1080.0f;
    ImU32 tint = Rgba((int)(255 * dim), (int)(255 * dim), (int)(255 * dim), a);

    XemuTexture cover = GameCover(game);
    if (cover) {
        dl->AddImageQuad((ImTextureID)cover, tl, tr, br, bl, ImVec2(0, 0),
                         ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
    } else {
        // Dark case, and the green band across the top of Xbox cases.
        dl->AddQuadFilled(tl, tr, br, bl,
                          Rgba((int)(16 * dim), (int)(52 * dim),
                               (int)(10 * dim), a));
        auto lerp = [](ImVec2 p, ImVec2 q, float t) {
            return ImVec2(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t);
        };
        ImVec2 bl2 = lerp(tl, bl, 0.12f), br2 = lerp(tr, br, 0.12f);
        dl->AddQuadFilled(tl, tr, br2, bl2,
                          Rgba((int)(90 * dim), (int)(190 * dim),
                               (int)(30 * dim), a));
        float band_h = bl2.y - tl.y;
        ImFont *font = g_font_mgr.m_menu_font_small;
        Text(dl, font, band_h * 0.7f,
             ImVec2(tl.x + (tr.x - tl.x) * 0.08f, tl.y + band_h * 0.15f),
             Rgba(10, 40, 6, a * dim), "XBOX");

        // The disc's logo in the middle, else a disc.
        ImVec2 c = lerp(lerp(tl, br, 0.5f), lerp(tr, bl, 0.5f), 0.5f);
        c.y += band_h * 0.4f;
        float side = std::min(tr.x - tl.x, bl.y - tl.y) * 0.62f;
        XemuTexture image = GameImage(game);
        if (image) {
            dl->AddImage((ImTextureID)image,
                         ImVec2(c.x - side / 2, c.y - side / 2),
                         ImVec2(c.x + side / 2, c.y + side / 2), ImVec2(0, 0),
                         ImVec2(1, 1), tint);
        } else {
            dl->AddCircleFilled(c, side * 0.45f,
                                Rgba((int)(30 * dim), (int)(110 * dim),
                                     (int)(12 * dim), a),
                                48);
            dl->AddCircle(c, side * 0.45f, Lime(a * dim), 48, 2 * s);
            dl->AddCircle(c, side * 0.14f, Lime(a * dim), 24, 2 * s);
        }
    }
    dl->AddQuad(tl, tr, br, bl, Line(a * dim * 0.9f), 1.5f * s);
    return cover;
}

// Games: a carousel of cases. The selected one big in the middle with its
// name and details under it; the others turned towards it on either side,
// stacked closer the further they are, all with a reflection.
void DashboardScene::DrawGames(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);

    if (m_games.empty()) {
        Text(dl, font, 56 * s, ImVec2(280 * s + slide, 330 * s), White(a),
             "No games yet");
        Text(dl, small, 34 * s, ImVec2(280 * s + slide, 410 * s), Label(a),
             "Put your .iso games in /data/xemu/games");
        return;
    }

    int n = m_games.size();
    float cx = size.x / 2 + slide, cy = 500 * s;
    const float center_h = 500 * s, side_h = 390 * s, aspect = 0.71f;
    const int reach = 6; // Cases drawn either side

    // Where a case d places from the selection sits (d fractional while
    // scrolling): its middle, height, width and how far it's turned.
    struct Slot {
        float x, h, w, turn, dim;
    };
    auto slot = [&](float d) {
        float t = std::min(fabsf(d), 1.f);
        float far = std::max(fabsf(d) - 1, 0.f);
        float sign = d < 0 ? -1 : 1;
        Slot sl;
        sl.h = center_h + (side_h - center_h) * t - far * 8 * s;
        sl.w = sl.h * aspect * (1 - 0.22f * t);
        sl.x = cx + sign * (t * 360 * s + far * 118 * s);
        sl.turn = sign * 0.09f * t; // Outer edge this much shorter
        sl.dim = 1 - 0.4f * t - 0.06f * std::min(far, 4.f);
        return sl;
    };

    // The glow behind the selection.
    for (int i = 4; i >= 1; i--) {
        float g = i * 26 * s;
        dl->AddRectFilled(
            ImVec2(cx - center_h * aspect / 2 - g, cy - center_h / 2 - g),
            ImVec2(cx + center_h * aspect / 2 + g, cy + center_h / 2 + g),
            Rgba(90, 200, 30, a * 0.035f), 40 * s + g);
    }

    // Farthest first, so nearer cases cover them.
    std::vector<int> order;
    int first = std::max(0, (int)floorf(m_game_anim) - reach);
    int last = std::min(n - 1, (int)ceilf(m_game_anim) + reach);
    for (int i = first; i <= last; i++) {
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](int i, int j) {
        return fabsf(i - m_game_anim) > fabsf(j - m_game_anim);
    });

    for (int i : order) {
        float d = i - m_game_anim;
        Slot sl = slot(d);
        float edge = fabsf(d) > reach - 1 ? reach - fabsf(d) : 1;
        float ca = a * std::clamp(edge, 0.f, 1.f);
        // Turned: the edge away from the middle is shorter.
        float hl = sl.h * (1 - std::max(-sl.turn, 0.f) * 2);
        float hr = sl.h * (1 - std::max(sl.turn, 0.f) * 2);
        float x0 = sl.x - sl.w / 2, x1 = sl.x + sl.w / 2;
        ImVec2 tl(x0, cy - hl / 2), bl(x0, cy + hl / 2);
        ImVec2 tr(x1, cy - hr / 2), br(x1, cy + hr / 2);
        XemuTexture cover =
            DrawCase(m_games[i], tl, tr, br, bl, ca, sl.dim);

        // The reflection: the bottom of the cover, flipped, fading out.
        if (cover) {
            float rk = 0.28f; // Of the case's height
            ImVec2 rbl(bl.x, bl.y + 4 * s + hl * rk);
            ImVec2 rbr(br.x, br.y + 4 * s + hr * rk);
            int v0 = dl->VtxBuffer.Size;
            dl->AddImageQuad((ImTextureID)cover, ImVec2(bl.x, bl.y + 4 * s),
                             ImVec2(br.x, br.y + 4 * s), rbr, rbl,
                             ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 1 - rk),
                             ImVec2(0, 1 - rk), White(ca * 0.22f * sl.dim));
            // Bottom corners (added third and fourth) fade to nothing.
            for (int v = v0 + 2; v < v0 + 4 && v < dl->VtxBuffer.Size; v++) {
                dl->VtxBuffer[v].col &= ~IM_COL32_A_MASK;
            }
        }
    }

    // The selection: its outline, name and details.
    const Game &game = m_games[m_game];
    float fw = center_h * aspect, fh = center_h;
    if (fabsf(m_game_anim - m_game) < 0.3f) {
        Focus(dl, ImVec2(cx - fw / 2, cy - fh / 2),
              ImVec2(cx + fw / 2, cy + fh / 2), s, m_time,
              a * (1 - fabsf(m_game_anim - m_game) / 0.3f));
    }

    float ty = cy + fh / 2 + 44 * s;
    ImVec2 ns = TextSize(font, 54 * s, game.name.c_str());
    if (ns.x > size.x - 200 * s) {
        ns = font->CalcTextSizeA(54 * s, FLT_MAX, size.x - 200 * s,
                                 game.name.c_str());
    }
    Text(dl, font, 54 * s, ImVec2(cx - ns.x / 2, ty), White(a),
         game.name.c_str(), size.x - 200 * s);

    // Details: title ID, play time, when, and chips for this game's own
    // settings.
    std::string details;
    if (game.title_id) {
        char id[16];
        snprintf(id, sizeof(id), "%08X", game.title_id);
        details = id;
    }
    GamePlay play = GamePlayGet(KeyOf(game));
    std::string played = play.last ? "Played " + GamePlayTimeText(play.seconds) +
                                         "  -  " + GamePlayLastText(play.last) :
                                     "Not played yet";
    details += details.empty() ? played : "     " + played;
    char count[32];
    snprintf(count, sizeof(count), "     %d / %d", m_game + 1, n);
    details += count;

    std::vector<std::string> chips;
    const GameProfile &profile = ProfileOf(game);
    for (int o = 0; o < GO__COUNT; o++) {
        if (profile.value[o] != GAME_DEFAULT) {
            chips.push_back(GameOptionValueName(o, profile.value[o]));
        }
    }
    float chip_pad = 14 * s, chip_gap = 10 * s;
    ImVec2 dsz = TextSize(small, 30 * s, details.c_str());
    float total = dsz.x;
    for (const auto &c : chips) {
        total += chip_gap + TextSize(small, 26 * s, c.c_str()).x + 2 * chip_pad;
    }
    float dy = ty + ns.y + 18 * s, dx = cx - total / 2;
    Text(dl, small, 30 * s, ImVec2(dx, dy), Label(a * 0.9f), details.c_str());
    dx += dsz.x;
    for (const auto &c : chips) {
        ImVec2 cs = TextSize(small, 26 * s, c.c_str());
        ImVec2 c0(dx + chip_gap, dy + 1 * s);
        ImVec2 c1(c0.x + cs.x + 2 * chip_pad, c0.y + dsz.y);
        dl->AddRectFilled(c0, c1, Rgba(40, 110, 15, a * 0.6f), dsz.y / 2);
        Text(dl, small, 26 * s,
             ImVec2(c0.x + chip_pad, c0.y + (dsz.y - cs.y) / 2), Lime(a),
             c.c_str());
        dx = c1.x;
    }
    if (game.full_disc) {
        const char *warn = "Full disc image: convert it to XISO to play it";
        ImVec2 ws = TextSize(small, 28 * s, warn);
        Text(dl, small, 28 * s, ImVec2(cx - ws.x / 2, dy + dsz.y + 10 * s),
             Lime(a), warn);
    }
}

// A game's picture in the box p0..p1: with cover, its box art if the user
// added one, else the title image from the disc, else a turning disc; in
// the list (cover false), the title image or a small disc.
void DashboardScene::DrawGameArt(const Game &game, ImVec2 p0, ImVec2 p1,
                                 float a, bool cover)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float bw = p1.x - p0.x, bh = p1.y - p0.y;
    ImVec2 c(p0.x + bw / 2, p0.y + bh / 2);
    float s = ImGui::GetIO().DisplaySize.y / 1080.0f;

    XemuTexture tex = cover ? GameCover(game) : 0;
    if (!tex) {
        tex = GameImage(game);
    }
    int tw, th;
    if (tex && xemu_vk_texture_get_size(tex, &tw, &th) && tw > 0 && th > 0) {
        // Fit inside the box, keeping its shape; covers stand on the
        // bottom edge, title images sit in the middle.
        float k = std::min(bw / tw, bh / th);
        ImVec2 size(tw * k, th * k);
        ImVec2 q0(c.x - size.x / 2,
                  cover && tex == Art(game.path).cover ? p1.y - size.y :
                                                         c.y - size.y / 2);
        ImVec2 q1(q0.x + size.x, q0.y + size.y);
        float round = (cover ? 10 : 6) * s;
        if (cover) {
            dl->AddRectFilled(ImVec2(q0.x - 14 * s, q0.y - 14 * s),
                              ImVec2(q1.x + 14 * s, q1.y + 14 * s),
                              Rgba(90, 200, 30, a * 0.12f), round * 2);
        }
        dl->AddImageRounded((ImTextureID)tex, q0, q1, ImVec2(0, 0),
                            ImVec2(1, 1), White(a), round);
        dl->AddRect(q0, q1, cover ? Lime(a * 0.8f) : Line(a * 0.8f), round, 0,
                    (cover ? 3 : 2) * s);
        return;
    }

    // No picture: a disc, turning in the big panel.
    float r = std::min(bw, bh) / 2 * (cover ? 0.85f : 0.9f);
    if (cover) {
        dl->AddCircleFilled(c, r * 1.2f, Rgba(90, 200, 30, a * 0.10f), 48);
    }
    dl->AddCircleFilled(c, r, Rgba(30, 110, 12, a), 64);
    dl->AddCircle(c, r, Lime(a), 64, (cover ? 3 : 2) * s);
    dl->AddCircle(c, r * 0.3f, Lime(a * 0.9f), 32, (cover ? 3 : 2) * s);
    dl->AddCircleFilled(c, r * 0.12f, Rgba(4, 30, 4, a), 24);
    if (cover) {
        float spin = m_time * 1.5f;
        dl->PathArcTo(c, r * 0.68f, spin, spin + 1.3f, 20);
        dl->PathStroke(White(a * 0.55f), 0, 6 * s);
        dl->PathArcTo(c, r * 0.68f, spin + M_PI, spin + M_PI + 0.8f, 16);
        dl->PathStroke(White(a * 0.35f), 0, 6 * s);
    }
}

void DashboardScene::DrawSettings(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);

    // GameOptionValueName reuses its buffer: copy each value out.
    std::string values[SR__COUNT];
    for (int i = 0; i < SR__COUNT; i++) {
        int option = kSettingOptions[i];
        if (option >= 0) {
            values[i] = GameOptionValueLabel(option, GameGlobal(option));
        }
    }
    values[SR_SOUNDS] = g_config.display.ui.menu_sounds ? "On" : "Off";
    bool scale_locked = GameAnyRunning();
    if (scale_locked) {
        values[SR_SCALE] += "  (locked)";
    }
    const struct {
        const char *name, *help;
    } rows[SR__COUNT] = {
        { "Resolution", "Higher is sharper, uses more GPU. Lines are for "
                        "480p games (720p ones go higher: 2x = 1440p)." },
        { "Screen shape",
          "Most games are 4:3. For a game in widescreen mode (Xbox setting), "
          "give it 16:9 with Triangle on it." },
        { "Picture fit", "Stretch fills the whole screen." },
        { "Smoothing", "Off keeps the pixels sharp." },
        { "Volume", "How loud the games are." },
        { "Menu sounds",
          "The dashboard's ticks and chimes. Your own: WAVs in "
          "/data/xemu/sounds." },
        { "Performance overlay",
          "Frame rate, CPU and memory in the corner while you play." },
        { "Advanced settings",
          "The Xbox's memory and video output, audio and performance." },
    };
    const int n = SR__COUNT;

    float x = 280 * s + slide, w = 1400 * s, h = 70 * s, gap = 84 * s;
    float y0 = 236 * s;
    for (int i = 0; i < n; i++) {
        float y = y0 + i * gap;
        bool selected = i == m_setting;
        bool locked = i == SR_SCALE && scale_locked;
        float ra = locked ? a * 0.55f : a;
        Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, ra);
        Text(dl, font, 42 * s, ImVec2(x + 40 * s, y + 12 * s),
             selected ? Ink(a) : Label(ra), rows[i].name);
        if (!values[i].empty()) {
            char value[80];
            snprintf(value, sizeof(value),
                     selected && !locked ? "<  %s  >" : "%s",
                     values[i].c_str());
            ImVec2 vs = TextSize(font, 42 * s, value);
            Text(dl, font, 42 * s, ImVec2(x + w - vs.x - 50 * s, y + 12 * s),
                 selected ? Ink(a) : White(ra * 0.9f), value);
        }
    }
    Focus(dl, ImVec2(x, y0 + m_setting * gap),
          ImVec2(x + w, y0 + m_setting * gap + h), s, m_time, a);

    // The selection's explanation, under the list.
    const char *help = rows[m_setting].help;
    if (m_setting == SR_SCALE && scale_locked) {
        help = "Locked while a game runs. Change it before starting one "
               "(or after the Xbox Dashboard button).";
    }
    Text(dl, small, 32 * s, ImVec2(x + 10 * s, y0 + n * gap + 14 * s),
         Label(a), help, w);
}

void DashboardScene::DrawAdvanced(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);

    auto on_off = [](bool on) { return on ? "On" : "Off"; };
    uint32_t video = 0;
    bool video_ok = xemu_eeprom_get_video_flags(&video);
    auto video_on = [&](uint32_t flag) {
        return video_ok ? on_off(video & flag) : "?";
    };
    bool hdtv = g_config.sys.avpack == CONFIG_SYS_AVPACK_HDTV;
    const struct {
        const char *name, *value, *help;
    } rows[ADV__COUNT] = {
        { "Xbox widescreen", video_on(XBOX_VIDEO_WIDESCREEN),
          "The Xbox's own 16:9 mode: games that have it draw a wider view "
          "(Halo 2, GTA San Andreas, Forza...). Give those games Screen shape "
          "16:9 (Triangle on them); others stay 4:3." },
        { "Xbox 480p", video_on(XBOX_VIDEO_480P),
          hdtv ? "Progressive 480 lines: sharper and steadier than 480i. "
                 "Nearly every game has it." :
                 "Needs Video output: HDTV (below)." },
        { "Xbox 720p", video_on(XBOX_VIDEO_720P),
          hdtv ? "Only games made for it use 720p (Soul Calibur II, Amped 2, "
                 "Crash Nitro Kart...); the rest keep 480p." :
                 "Needs Video output: HDTV (below)." },
        { "Xbox 1080i", video_on(XBOX_VIDEO_1080I),
          hdtv ? "Interlaced, and very few games have it: usually best Off." :
                 "Needs Video output: HDTV (below)." },
        { "Xbox memory",
          g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128 ? "128 MB" :
                                                               "64 MB",
          "64 MB like a retail Xbox: what every game is made for. (128 MB "
          "developer-kit mode is off on the PS5.)" },
        { "Video output", AvpackName(g_config.sys.avpack),
          "HDTV lets games use 480p, 720p and 1080i (the Xbox 480p/720p/"
          "1080i rows above)." },
        { "Skip boot animation", on_off(g_config.general.skip_boot_anim),
          "Starts games without the Xbox logo animation." },
        { "Audio processor (DSP)", on_off(GameGlobal(GO_DSP)),
          "Keep it Off: on the PS5 it's very slow and games stall (Halo 2 "
          "freezes, GTA can't read the disc). Only for a game with no sound." },
        { "Fast floating point", on_off(g_config.perf.hard_fpu),
          "Faster. Turn it off if a game's physics or animation go wrong." },
        { "Shader cache", on_off(g_config.perf.cache_shaders),
          "Keeps compiled graphics shaders, so games stutter less next time." },
        { "Notifications", on_off(g_config.display.ui.show_notifications),
          "The small messages in the corner of the screen." },
        { "CPU core pinning", on_off(g_config.perf.cpu_pinning),
          "The Xbox's CPU and GPU each get a PS5 core of their own. Turn it "
          "off to compare." },
        { "Profiler", on_off(g_config.perf.profiler),
          "Notes where the time goes, for xemu-game.log (and its CPU "
          "numbers). Off if a game misbehaves with it." },
    };

    float x = 280 * s + slide, w = 1400 * s, h = 62 * s, gap = 74 * s;
    float y0 = 226 * s;
    const int visible = 8;
    int top = std::clamp(m_adv_setting - visible / 2, 0,
                         std::max(0, ADV__COUNT - visible));
    for (int i = top; i < std::min((int)ADV__COUNT, top + visible); i++) {
        float y = y0 + (i - top) * gap;
        bool selected = i == m_adv_setting;
        bool locked = AdvancedLocked(i);
        float ra = locked ? a * 0.55f : a; // Locked rows are dimmed
        Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, ra);
        char name[64];
        snprintf(name, sizeof(name), "%s%s", rows[i].name,
                 AdvancedNeedsRestart(i) ? " *" : "");
        Text(dl, font, 40 * s, ImVec2(x + 40 * s, y + 9 * s),
             selected ? Ink(a) : Label(ra), name);
        char value[48];
        snprintf(value, sizeof(value),
                 locked ? "%s  (locked)" : selected ? "<  %s  >" : "%s",
                 rows[i].value);
        ImVec2 vs = TextSize(font, 40 * s, value);
        Text(dl, font, 40 * s, ImVec2(x + w - vs.x - 50 * s, y + 9 * s),
             selected ? Ink(a) : White(ra * 0.9f), value);
    }
    Focus(dl, ImVec2(x, y0 + (m_adv_setting - top) * gap),
          ImVec2(x + w, y0 + (m_adv_setting - top) * gap + h), s, m_time, a);
    if (top > 0) {
        Text(dl, small, 30 * s, ImVec2(x + w + 20 * s, y0 + 10 * s), Lime(a),
             "^");
    }
    if (top + visible < ADV__COUNT) {
        Text(dl, small, 30 * s,
             ImVec2(x + w + 20 * s, y0 + (visible - 1) * gap + 20 * s),
             Lime(a), "v");
    }

    float ty = y0 + visible * gap + 16 * s;
    Text(dl, small, 32 * s, ImVec2(x + 10 * s, ty), Label(a),
         rows[m_adv_setting].help, w);
    Text(dl, small, 30 * s, ImVec2(x + 10 * s, ty + 94 * s),
         m_restart_needed ? Lime(a) : Label(a * 0.6f),
         m_restart_needed ?
             "* Changed: close and reopen XPSemu to apply." :
             "* Takes effect the next time XPSemu starts.");
}

void DashboardScene::DrawSystem(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);
    float x = 280 * s + slide, y = 240 * s;

    auto file = [](const char *path) {
        const char *slash = path ? strrchr(path, '/') : NULL;
        return slash ? slash + 1 : (path && path[0] ? path : "(none)");
    };
    char ram[32], games[64], version[64];
    snprintf(ram, sizeof(ram), "%s MB",
             g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128 ? "128" : "64");
    snprintf(games, sizeof(games), "%d in %s", (int)m_games.size(),
             g_config.general.games_dir ? g_config.general.games_dir : "");
    snprintf(version, sizeof(version), "XPSemu, based on xemu %s", xemu_version);
    const char *dvd = g_config.sys.files.dvd_path;
    const struct { const char *name, *value; } info[] = {
        { "Xbox RAM", ram },
        { "MCPX boot ROM", file(g_config.sys.files.bootrom_path) },
        { "BIOS", file(g_config.sys.files.flashrom_path) },
        { "Hard disk", file(g_config.sys.files.hdd_path) },
        { "Disc", dvd && dvd[0] ? file(dvd) : "(empty)" },
        { "Games", games },
        { "GPU", pgraph_vk_get_device_name() },
        { "Version", version },
    };
    for (int i = 0; i < (int)(sizeof(info) / sizeof(info[0])); i++) {
        float ry = y + i * 58 * s;
        Text(dl, small, 34 * s, ImVec2(x + 10 * s, ry), Label(a * 0.9f),
             info[i].name);
        Text(dl, small, 34 * s, ImVec2(x + 400 * s, ry), White(a * 0.9f),
             info[i].value);
    }

    static const char *const actions[] = { "Restart the Xbox", "Eject the disc" };
    float ay = y + 8 * 58 * s + 50 * s, w = 800 * s, h = 84 * s, gap = 104 * s;
    for (int i = 0; i < 2; i++) {
        float by = ay + i * gap;
        bool selected = i == m_system_row;
        Bar(dl, ImVec2(x, by), ImVec2(x + w, by + h), s, selected, a);
        Text(dl, font, 44 * s, ImVec2(x + 40 * s, by + 18 * s),
             selected ? Ink(a) : Label(a), actions[i]);
    }
    float fy = ay + m_system_row * gap;
    Focus(dl, ImVec2(x, fy), ImVec2(x + w, fy + h), s, m_time, a);
}

void DashboardScene::DrawHints(float s)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float a = m_alpha;

    struct Hint { Glyph glyph; const char *text; };
    std::vector<Hint> hints;
    if (!m_in_page) {
        hints.push_back({ GLYPH_CROSS, m_shelf ? "Play" :
                                       m_page == MENU_XBOX_DASHBOARD ?
                                                 "Start" :
                                                 "Open" });
        if (m_shelf) {
            hints.push_back({ GLYPH_TRIANGLE, "Game settings" });
        }
        if (m_can_return) {
            hints.push_back({ GLYPH_CIRCLE, m_game_started ?
                                                "Back to the game" :
                                                "Back to the Xbox" });
        }
    } else {
        if (m_gs_open) {
            hints.push_back({ GLYPH_CROSS,
                              m_gs_patches ? "Turn on/off" :
                              m_gs_row == GS_RESET ? "Reset" :
                              m_gs_row == GS_PATCHES ? "Open" :
                                                       "Change" });
        } else if (m_page == PAGE_GAMES && !m_games.empty()) {
            hints.push_back({ GLYPH_CROSS, "Play" });
            hints.push_back({ GLYPH_TRIANGLE, "Game settings" });
        } else if (m_page == PAGE_SETTINGS) {
            if (!m_advanced || !AdvancedLocked(m_adv_setting)) {
                hints.push_back({ GLYPH_CROSS,
                                  !m_advanced && m_setting == SR_ADVANCED ?
                                      "Open" :
                                                                  "Change" });
            }
        } else if (m_page == PAGE_SYSTEM) {
            hints.push_back({ GLYPH_CROSS, "Select" });
        }
        hints.push_back({ GLYPH_CIRCLE, "Back" });
    }

    float x = 90 * s, y = size.y - 80 * s;
    for (const auto &h : hints) {
        DrawGlyph(dl, ImVec2(x + 22 * s, y), 22 * s, h.glyph, a);
        Text(dl, small, 32 * s, ImVec2(x + 56 * s, y - 18 * s),
             White(a * 0.85f), h.text);
        x += 100 * s + TextSize(small, 32 * s, h.text).x;
    }

    // The time, bottom right.
    char clock[16];
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    if (lt) {
        strftime(clock, sizeof(clock), "%H:%M", lt);
        ImVec2 cs = TextSize(small, 34 * s, clock);
        Text(dl, small, 34 * s, ImVec2(size.x - cs.x - 90 * s, y - 20 * s),
             Label(a * 0.9f), clock);
    }
}

//
// Performance overlay
//

#ifdef __PROSPERO__
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t *available);
#endif

void DrawPerfOverlay()
{
    int mode = g_config.display.ui.perf_overlay;
    if (mode == CONFIG_DISPLAY_UI_PERF_OVERLAY_OFF ||
        g_scene_mgr.IsDisplayingScene()) {
        return;
    }

    // The slow numbers once a second: emulator CPU time as a share of one
    // PS5 core (xemu uses several threads, so it can pass 100%), and free
    // memory.
    static double last_sample = -1, last_cpu;
    static int cpu_percent = -1;
    static long free_mb = -1;
    double now = ImGui::GetTime();
    if (last_sample < 0 || now - last_sample >= 1.0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
            double cpu = ts.tv_sec + ts.tv_nsec / 1e9;
            if (last_sample >= 0) {
                cpu_percent =
                    (int)lround((cpu - last_cpu) / (now - last_sample) * 100);
            }
            last_cpu = cpu;
        }
#ifdef __PROSPERO__
        size_t available = 0;
        if (sceKernelAvailableFlexibleMemorySize(&available) == 0) {
            free_mb = (long)(available >> 20);
        }
#endif
        last_sample = now;
    }

    unsigned int fps = g_nv2a_stats.increment_fps;
    int mspf = g_nv2a_stats.frame_history[(g_nv2a_stats.frame_ptr +
                                           NV2A_PROF_NUM_FRAMES - 1) %
                                          NV2A_PROF_NUM_FRAMES].mspf;
    std::vector<std::string> lines;
    char buf[96];
    snprintf(buf, sizeof(buf), "%u FPS", fps);
    lines.push_back(buf);
    if (mode == CONFIG_DISPLAY_UI_PERF_OVERLAY_FULL) {
        snprintf(buf, sizeof(buf), "Frame time  %d ms", mspf);
        lines.push_back(buf);
        snprintf(buf, sizeof(buf), "Display  %.0f FPS",
                 ImGui::GetIO().Framerate);
        lines.push_back(buf);
        if (cpu_percent >= 0) {
            snprintf(buf, sizeof(buf), "CPU  %d%% of a core", cpu_percent);
            lines.push_back(buf);
        }
        if (free_mb >= 0) {
            snprintf(buf, sizeof(buf), "Free memory  %ld MB", free_mb);
            lines.push_back(buf);
        }
        snprintf(buf, sizeof(buf), "Xbox %s MB  -  %dx resolution",
                 g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128 ? "128" :
                                                                      "64",
                 nv2a_get_surface_scale_factor());
        lines.push_back(buf);
    }

    ImDrawList *dl = ImGui::GetForegroundDrawList();
    float s = ImGui::GetIO().DisplaySize.y / 1080.0f;
    ImFont *font = g_font_mgr.m_menu_font_small;
    float size = 28 * s, pad = 14 * s, line = 34 * s;
    float w = 0;
    for (const auto &l : lines) {
        w = std::max(w, TextSize(font, size, l.c_str()).x);
    }
    ImVec2 p0(30 * s, 30 * s);
    ImVec2 p1(p0.x + w + 2 * pad, p0.y + lines.size() * line + 2 * pad - 6 * s);
    dl->AddRectFilled(p0, p1, Rgba(0, 20, 0, 0.6f), 10 * s);
    // The frame rate: green at full speed (many Xbox games run at 30),
    // yellow when slow, red when very slow.
    ImU32 fps_col = fps >= 28 ? Lime(1) : fps >= 18 ? Rgba(255, 210, 60, 1) :
                                                      Rgba(255, 90, 70, 1);
    for (size_t i = 0; i < lines.size(); i++) {
        Text(dl, font, size, ImVec2(p0.x + pad, p0.y + pad + i * line),
             i == 0 ? fps_col : White(0.9f), lines[i].c_str());
    }
}

void DashboardTick()
{
    GameLogTick();
    g_dashboard.WatchTick();
    GamePlayTick(ImGui::GetIO().DeltaTime,
                 runstate_is_running() && !g_scene_mgr.IsDisplayingScene());
}
