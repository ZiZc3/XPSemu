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
#include <deque>
#include <set>
#include <mutex>
#include <thread>

#include "common.hh"
#include "actions.hh"
#include "hw/xbox/smbus.h"
#include "dashboard.hh"
#include "font-manager.hh"
#include "input-manager.hh"
#include "main-menu.hh"
#include "scene-manager.hh"
#include "game-log.hh"
#include "game-profile.hh"
#include "bc7-encode.h"
#include "cover-download.hh"
#include "ui-sounds.hh"
#include "xiso.hh"
#include <fpng.h>
#include "../xemu-notifications.h"
#include "../../xemu-xbe.h"
#include "../xemu-snapshots.h"
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

// Fades the vertices added since v0 from their alpha at y0 to none at y1:
// a reflection that thins out downwards, whatever shape drew it.
static void FadeDown(ImDrawList *dl, int v0, float y0, float y1)
{
    for (int v = v0; v < dl->VtxBuffer.Size; v++) {
        ImDrawVert &vx = dl->VtxBuffer[v];
        float t = std::clamp((vx.pos.y - y0) / std::max(y1 - y0, 1.0f), 0.0f,
                             1.0f);
        ImU32 alpha = (vx.col >> IM_COL32_A_SHIFT) & 0xFF;
        alpha = (ImU32)(alpha * (1 - t));
        vx.col = (vx.col & ~IM_COL32_A_MASK) | (alpha << IM_COL32_A_SHIFT);
    }
}

// A picture filling the screen (cropped to it, not stretched), blurred by
// drawing it a dozen times around a circle, averaged; drifting slowly.
static void DrawBlurredFill(ImDrawList *dl, XemuTexture tex, bool flip,
                            ImVec2 size, float radius, float t, float a)
{
    int tw = 4, th = 3;
    xemu_vk_texture_get_size(tex, &tw, &th);
    float image = th > 0 ? (float)tw / th : 4 / 3.0f;
    image = image > 1.5f ? image : 4 / 3.0f; // The Xbox's pictures are 4:3
    float screen = size.x / size.y;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (image < screen) { // Crop top and bottom
        float k = image / screen;
        uv0.y = (1 - k) / 2;
        uv1.y = 1 - uv0.y;
    } else {
        float k = screen / image;
        uv0.x = (1 - k) / 2;
        uv1.x = 1 - uv0.x;
    }
    if (flip) {
        std::swap(uv0.y, uv1.y);
    }
    float zoom = 1.08f + 0.02f * sinf(t * 0.07f);
    ImVec2 c(size.x / 2 + sinf(t * 0.05f) * size.x * 0.01f, size.y / 2);
    ImVec2 half(size.x * zoom / 2, size.y * zoom / 2);
    static const float ring[][2] = {
        { 0, 0 },          { 0.5f, 0 },       { -0.5f, 0 },
        { 0.25f, 0.43f },  { -0.25f, 0.43f }, { 0.25f, -0.43f },
        { -0.25f, -0.43f }, { 0.87f, 0.5f },  { -0.87f, 0.5f },
        { 0.87f, -0.5f },  { -0.87f, -0.5f }, { 0, 1 },
        { 0, -1 },
    };
    int n = sizeof(ring) / sizeof(ring[0]);
    for (int i = 0; i < n; i++) {
        ImVec2 o(ring[i][0] * radius, ring[i][1] * radius);
        // Each layer 1/(i+1) over the ones before: an even average.
        dl->AddImage((ImTextureID)tex,
                     ImVec2(c.x - half.x + o.x, c.y - half.y + o.y),
                     ImVec2(c.x + half.x + o.x, c.y + half.y + o.y), uv0, uv1,
                     IM_COL32(255, 255, 255, (int)(255 * a / (i + 1))));
    }
}

static ImVec2 TextSize(ImFont *font, float size, const char *text)
{
    return font->CalcTextSizeA(size, FLT_MAX, 0, text);
}

// Where the button hints along the bottom begin: text stays above it.
static float HintsTop()
{
    ImVec2 size = ImGui::GetIO().DisplaySize;
    return size.y - 118 * (size.y / 1080.0f);
}

// Wrapped text that ends above max_y (cut short with "..." if it would go
// past), returning where the next line can start.
static float TextFit(ImDrawList *dl, ImFont *font, float size, ImVec2 pos,
                     ImU32 col, const std::string &text, float wrap,
                     float max_y = 0)
{
    if (max_y <= 0) {
        max_y = HintsTop();
    }
    std::string fit = text;
    auto height = [&](const std::string &s) {
        return font->CalcTextSizeA(size, FLT_MAX, wrap, s.c_str()).y;
    };
    while (!fit.empty() && pos.y + height(fit) > max_y) {
        size_t cut = fit.rfind(' ', fit.size() > 4 ? fit.size() - 4 : 0);
        if (cut == std::string::npos || cut == 0) {
            fit.clear();
            break;
        }
        fit = fit.substr(0, cut) + "...";
    }
    if (fit.empty()) {
        return pos.y;
    }
    dl->AddText(font, size, pos, col, fit.c_str(), NULL, wrap);
    return pos.y + height(fit);
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
enum Glyph { GLYPH_CROSS, GLYPH_CIRCLE, GLYPH_TRIANGLE, GLYPH_SQUARE,
             GLYPH_LSTICK, GLYPH_RSTICK };

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
    case GLYPH_LSTICK: // A stick seen from above, with its letter
    case GLYPH_RSTICK: {
        dl->AddCircleFilled(c, r * 0.72f, White(a * 0.18f), 24);
        const char *letter = g == GLYPH_LSTICK ? "L" : "R";
        ImFont *font = g_font_mgr.m_menu_font_small;
        ImVec2 ls = font->CalcTextSizeA(r * 1.15f, FLT_MAX, 0, letter);
        dl->AddText(font, r * 1.15f, ImVec2(c.x - ls.x / 2, c.y - ls.y / 2), col,
                    letter);
        break;
    }
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
    double cover_time = 0; // When the cover came (it fades in)
    XemuTexture back = 0, spine = 0; // For the Box Art Viewer
    bool back_tried = false, spine_tried = false;
    double part_time = 0;
    double parts_asked = -100; // When the back/spine were asked for
};

static std::map<std::string, GameArt> g_art; // By disc image path
static int g_texture_budget;                  // Textures left this frame

static GameArt &Art(const std::string &path)
{
    return g_art[path];
}

//
// Pictures decode on threads of their own: a cover from xdb is a full-size
// scan (about 1500x2100), and decoding and shrinking it took the UI thread
// tens of milliseconds, two a frame, so the Games row stuttered until all
// were in. Now the UI only uploads the small result (a few a frame), and
// every cover starts decoding as soon as the games are listed.
//

struct DecodeJob {
    std::string game_path, file;
    int max_h, part; // part: CoverPart
};
struct Decoded {
    DecodeJob job;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};
static std::mutex g_decode_lock;
static std::deque<DecodeJob> g_decode_queue;
static std::deque<Decoded> g_decoded;
static int g_decode_threads;

// file as RGBA, at most max_h rows (box filter: each output pixel averages
// the ones under it).
static bool DecodePicture(const std::string &file, int max_h,
                          std::vector<uint8_t> *out, int *ow, int *oh)
{
    int w, h, channels;
    unsigned char *data = stbi_load(file.c_str(), &w, &h, &channels, 4);
    if (!data) {
        return false;
    }
    if (h > max_h) {
        int dw = std::max(1, w * max_h / h), dh = max_h;
        out->resize((size_t)dw * dh * 4);
        for (int y = 0; y < dh; y++) {
            int y0 = y * h / dh, y1 = std::max(y0 + 1, (y + 1) * h / dh);
            for (int x = 0; x < dw; x++) {
                int x0 = x * w / dw, x1 = std::max(x0 + 1, (x + 1) * w / dw);
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
                    (*out)[((size_t)y * dw + x) * 4 + c] = sum[c] / n;
                }
            }
        }
        *ow = dw, *oh = dh;
    } else {
        out->assign(data, data + (size_t)w * h * 4);
        *ow = w, *oh = h;
    }
    stbi_image_free(data);
    return true;
}

static void DecodeWorker()
{
    for (;;) {
        DecodeJob job;
        {
            std::lock_guard<std::mutex> guard(g_decode_lock);
            if (g_decode_queue.empty()) {
                g_decode_threads--;
                return;
            }
            job = g_decode_queue.front();
            g_decode_queue.pop_front();
        }
        Decoded done;
        done.job = job;
        DecodePicture(job.file, job.max_h, &done.rgba, &done.w, &done.h);
        std::lock_guard<std::mutex> guard(g_decode_lock);
        g_decoded.push_back(std::move(done));
    }
}

static void DecodeRequest(const DecodeJob &job)
{
    std::lock_guard<std::mutex> guard(g_decode_lock);
    g_decode_queue.push_back(job);
    if (g_decode_threads < 2) { // Two of the spare cores
        g_decode_threads++;
        std::thread(DecodeWorker).detach();
    }
}

// On the UI thread, each frame: up to a few decoded pictures become
// textures (they fade in).
static void UploadDecoded()
{
    for (int i = 0; i < 3; i++) {
        Decoded d;
        {
            std::lock_guard<std::mutex> guard(g_decode_lock);
            if (g_decoded.empty()) {
                return;
            }
            d = std::move(g_decoded.front());
            g_decoded.pop_front();
        }
        GameArt &art = g_art[d.job.game_path];
        XemuTexture tex = d.rgba.empty() ? 0 :
                          xemu_vk_texture_create(d.rgba.data(), d.w, d.h, 4);
        if (d.job.part == COVER_FRONT) {
            art.cover = tex;
            art.cover_time = ImGui::GetTime();
            fprintf(stderr, "XPSemu: cover %s%s\n", d.job.file.c_str(),
                    tex ? "" : " FAILED (not a picture stb_image reads)");
        } else {
            (d.job.part == COVER_BACK ? art.back : art.spine) = tex;
            art.part_time = ImGui::GetTime();
        }
    }
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

static std::vector<std::string> GameDirs(); // Below

static void ListCovers()
{
    g_covers.clear();
    std::vector<std::string> dirs;
    for (const auto &games : GameDirs()) { // Each one's covers folder, first
        dirs.push_back(games + "/covers");
    }
    dirs.push_back("/data/xemu/covers");
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

// The names a game's cover picture may have (see CoverKey).
static std::vector<std::string> CoverKeys(const DashboardScene::Game &game)
{
    GameArt &art = Art(game.path);
    std::vector<std::string> keys = { CoverKey(art.stem), CoverKey(game.name),
                                      CoverKey(art.info.title) };
    if (game.title_id) {
        char id[16];
        snprintf(id, sizeof(id), "%08x", game.title_id);
        keys.push_back(id);
    }
    return keys;
}

static bool HasCover(const DashboardScene::Game &game)
{
    for (const auto &key : CoverKeys(game)) {
        if (!key.empty() && g_covers.count(key)) {
            return true;
        }
    }
    return false;
}

static XemuTexture GameCover(const DashboardScene::Game &game)
{
    GameArt &art = Art(game.path);
    if (!art.cover_tried) {
        art.cover_tried = true;
        for (const auto &key : CoverKeys(game)) {
            auto it = g_covers.find(key);
            if (key.empty() || it == g_covers.end()) {
                continue;
            }
            // Decoded meanwhile (UploadDecoded puts it in).
            DecodeRequest({ game.path, it->second, 720, COVER_FRONT });
            break;
        }
    }
    return art.cover;
}

//
// Scene
//

//
// Games' own names (Game settings > Name): /data/xemu/game-names.txt, one
// "<key>\t<name>" per line (the key as KeyOf); shown everywhere the game is.
//

static std::map<std::string, std::string> g_game_names;
static bool g_game_names_loaded;

static std::string GameNamesFile()
{
    return std::string(xemu_settings_get_base_path()) + "game-names.txt";
}

static void LoadGameNames()
{
    g_game_names_loaded = true;
    g_game_names.clear();
    std::ifstream f(GameNamesFile());
    std::string line;
    while (std::getline(f, line)) {
        size_t tab = line.find('\t');
        if (tab != std::string::npos && tab > 0 && tab + 1 < line.size()) {
            g_game_names[line.substr(0, tab)] = line.substr(tab + 1);
        }
    }
}

// name "" puts the game's own name back.
static void SaveGameName(const std::string &key, const std::string &name)
{
    if (!g_game_names_loaded) {
        LoadGameNames();
    }
    if (name.empty()) {
        g_game_names.erase(key);
    } else {
        g_game_names[key] = name;
    }
    FILE *f = fopen(GameNamesFile().c_str(), "w");
    if (!f) {
        return;
    }
    for (const auto &[k, v] : g_game_names) {
        fprintf(f, "%s\t%s\n", k.c_str(), v.c_str());
    }
    fclose(f);
}

static const std::string *GameOwnName(const std::string &key)
{
    if (!g_game_names_loaded) {
        LoadGameNames();
    }
    auto it = g_game_names.find(key);
    return it == g_game_names.end() ? nullptr : &it->second;
}

//
// The on-screen keyboard for a game's name: letters and digits in rows,
// then Shift, Space, Delete, Clear and Done. X types, Square deletes,
// Triangle is a space, Circle leaves it as it was. Done with nothing
// written gives the game its own name back.
//

static const char *const kKeyRows[4] = { "1234567890-", "qwertyuiop'",
                                         "asdfghjkl:&", "zxcvbnm,.!?" };
static const char *const kKeySpecial[5] = { "Shift", "Space", "Delete",
                                            "Clear", "Done" };
static const int kKeyCols = 11, kKeyMaxLen = 48;

void DashboardScene::OpenNameEditor()
{
    m_kb_open = true;
    const std::string *own = GameOwnName(KeyOf(m_gs_game));
    m_kb_text = own ? *own : m_gs_game.name;
    m_kb_row = 4;
    m_kb_col = 4; // Done
    m_kb_shift = false;
}

void DashboardScene::KeyboardInput(bool accept, bool back, bool square,
                                   bool options, bool up, bool down, bool left,
                                   bool right)
{
    auto pop_char = [&]() { // A whole UTF-8 character
        while (!m_kb_text.empty() &&
               ((unsigned char)m_kb_text.back() & 0xC0) == 0x80) {
            m_kb_text.pop_back();
        }
        if (!m_kb_text.empty()) {
            m_kb_text.pop_back();
        }
    };
    if (back) {
        m_kb_open = false;
        return;
    }
    int cols = m_kb_row < 4 ? kKeyCols : 5;
    if (up || down) {
        int row = (m_kb_row + (up ? 4 : 1)) % 5;
        // Keep about the same place across the wide bottom keys.
        float at = (m_kb_col + 0.5f) / cols;
        m_kb_col = std::min((int)(at * (row < 4 ? kKeyCols : 5)),
                            (row < 4 ? kKeyCols : 5) - 1);
        m_kb_row = row;
    }
    cols = m_kb_row < 4 ? kKeyCols : 5;
    if (left) m_kb_col = (m_kb_col + cols - 1) % cols;
    if (right) m_kb_col = (m_kb_col + 1) % cols;
    if (square) {
        pop_char();
    }
    if (options && (int)m_kb_text.size() < kKeyMaxLen) {
        m_kb_text += ' ';
    }
    if (!accept) {
        return;
    }
    if (m_kb_row < 4) {
        if ((int)m_kb_text.size() < kKeyMaxLen) {
            char c = kKeyRows[m_kb_row][m_kb_col];
            // Shift, or the first letter of a word: a capital.
            bool cap = m_kb_shift || m_kb_text.empty() || m_kb_text.back() == ' ';
            m_kb_text += cap ? (char)toupper(c) : c;
            m_kb_shift = false;
        }
        UiSoundPlay(UI_SOUND_CHANGE);
        return;
    }
    switch (m_kb_col) {
    case 0: m_kb_shift = !m_kb_shift; break;
    case 1:
        if ((int)m_kb_text.size() < kKeyMaxLen) {
            m_kb_text += ' ';
        }
        break;
    case 2: pop_char(); break;
    case 3: m_kb_text.clear(); break;
    default: { // Done
        std::string name = m_kb_text;
        while (!name.empty() && name.back() == ' ') {
            name.pop_back();
        }
        while (!name.empty() && name.front() == ' ') {
            name.erase(0, 1);
        }
        std::string key = KeyOf(m_gs_game);
        GameArt &art = Art(m_gs_game.path);
        std::string shown = name.empty() ? CleanName(art.stem) : name;
        SaveGameName(key, name == CleanName(art.stem) ? "" : name);
        for (auto &g : m_games) {
            if (g.path == m_gs_game.path) {
                g.name = shown;
            }
        }
        m_gs_game.name = shown;
        m_kb_open = false;
        UiSoundPlay(UI_SOUND_SELECT);
        break;
    }
    }
}

void DashboardScene::DrawKeyboard(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    dl->AddRectFilled(ImVec2(0, 0), size, Rgba(0, 0, 0, a * 0.55f));
    float pw = 1120 * s, ph = 600 * s;
    ImVec2 p0((size.x - pw) / 2, (size.y - ph) / 2 - 20 * s);
    ImVec2 p1(p0.x + pw, p0.y + ph);
    dl->AddRectFilled(p0, p1, Rgba(6, 24, 6, a * 0.97f), 22 * s);
    dl->AddRect(p0, p1, Lime(a * 0.8f), 22 * s, 0, 2 * s);
    Text(dl, small, 30 * s, ImVec2(p0.x + 40 * s, p0.y + 26 * s), Label(a),
         "Name");

    // The text, with a blinking cursor.
    ImVec2 f0(p0.x + 40 * s, p0.y + 70 * s), f1(p1.x - 40 * s, p0.y + 140 * s);
    dl->AddRectFilled(f0, f1, Rgba(0, 0, 0, a * 0.5f), 12 * s);
    dl->AddRect(f0, f1, Line(a), 12 * s, 0, 2 * s);
    std::string shown = m_kb_text;
    float fs = 42 * s, room = f1.x - f0.x - 40 * s;
    while (!shown.empty() && TextSize(font, fs, shown.c_str()).x > room) {
        shown.erase(0, 1); // The end shows when it's long
    }
    ImVec2 ts = TextSize(font, fs, shown.c_str());
    Text(dl, font, fs, ImVec2(f0.x + 20 * s, f0.y + 12 * s), White(a), shown.c_str());
    if (fmodf(m_time, 1.0f) < 0.6f) {
        float cx = f0.x + 22 * s + ts.x;
        dl->AddLine(ImVec2(cx, f0.y + 16 * s), ImVec2(cx, f1.y - 16 * s), Lime(a),
                    3 * s);
    }

    // The keys.
    float kx = p0.x + 40 * s, ky = p0.y + 170 * s, kw = (pw - 80 * s) / kKeyCols;
    float kh = 66 * s, gap = 8 * s;
    for (int r = 0; r < 5; r++) {
        int cols = r < 4 ? kKeyCols : 5;
        float w = r < 4 ? kw : (pw - 80 * s) / 5;
        for (int c = 0; c < cols; c++) {
            ImVec2 k0(kx + c * w + gap / 2, ky + r * (kh + gap));
            ImVec2 k1(k0.x + w - gap, k0.y + kh);
            bool sel = r == m_kb_row && c == m_kb_col;
            bool lit = r == 4 && c == 0 && m_kb_shift;
            dl->AddRectFilled(k0, k1, sel ? Lime(a) : Rgba(20, 52, 16, a * 0.9f),
                              10 * s);
            if (lit && !sel) {
                dl->AddRect(k0, k1, Lime(a), 10 * s, 0, 2 * s);
            }
            char one[2] = { r < 4 ? kKeyRows[r][c] : (char)0, 0 };
            if (r < 4 && m_kb_shift) {
                one[0] = (char)toupper(one[0]);
            }
            const char *label = r < 4 ? one : kKeySpecial[c];
            ImVec2 ls = TextSize(r < 4 ? font : small, r < 4 ? 38 * s : 30 * s, label);
            Text(dl, r < 4 ? font : small, r < 4 ? 38 * s : 30 * s,
                 ImVec2((k0.x + k1.x - ls.x) / 2, (k0.y + k1.y - ls.y) / 2),
                 sel ? Ink(a) : White(a * 0.92f), label);
        }
    }
}

// Where games are: games_dir, then xemu/games on each USB or extended
// drive (the slots VLC for the PS5 looks in). Watched while the dashboard
// is up, so drives come and go without restarting.
static std::vector<std::string> GameDirs()
{
    std::vector<std::string> dirs;
    const char *dir = g_config.general.games_dir;
    if (dir && dir[0]) {
        dirs.push_back(dir);
    }
#ifdef __PROSPERO__
    std::error_code ec;
    for (const char *drive : { "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/usb3",
                               "/mnt/usb4", "/mnt/usb5", "/mnt/usb6", "/mnt/usb7",
                               "/mnt/ext0", "/mnt/ext1" }) {
        std::string games = std::string(drive) + "/xemu/games";
        if (std::filesystem::is_directory(games, ec)) {
            dirs.push_back(games);
        }
    }
#endif
    return dirs;
}

// What the games folders look like now: each one there, and when it last
// changed (a game added or removed). A USB drive plugged in or pulled out,
// or a game copied over FTP, changes it.
static std::string GamesSignature()
{
    std::string sig;
    for (const std::string &dir : GameDirs()) {
        struct stat st;
        if (stat(dir.c_str(), &st) == 0) {
            sig += dir + ":" + std::to_string((long long)st.st_mtime) + ";";
        }
    }
    return sig;
}

void DashboardScene::ScanGames()
{
    m_games_sig = GamesSignature();
    LoadGameNames();
    m_games.clear();
    ListCovers();
    for (auto &[path, art] : g_art) {
        if (!art.cover) {
            art.cover_tried = false;
        }
    }
    std::error_code ec;
    for (const std::string &dir : GameDirs()) {
        if (!std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        if (dir == g_config.general.games_dir) {
            std::filesystem::create_directory(dir + "/covers", ec);
        } else {
            fprintf(stderr, "XPSemu: games on a drive: %s\n", dir.c_str());
        }
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
                const std::string *own = GameOwnName(KeyOf(m_games.back()));
                if (own) { // Its name as the user gave it
                    m_games.back().name = *own;
                }
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
    m_game_vel = 0;
    for (const auto &game : m_games) {
        GameCover(game); // Starts decoding it, if it has one
    }
    RefreshPatchBadges();
    RequestMissingCovers();
}

// Games without a cover get theirs from xdb by title ID, in the background
// (cover-download.cc), into /data/xemu/covers where ListCovers looks.
void DashboardScene::RequestMissingCovers()
{
    if (!g_config.display.ui.download_covers) {
        return;
    }
    std::vector<CoverWant> wants;
    for (const auto &game : m_games) {
        if (game.title_id && !HasCover(game)) {
            const GameArt &art = Art(game.path);
            wants.push_back({ game.title_id,
                              { game.name, art.info.title, art.stem },
                              COVER_FRONT });
        }
    }
    CoverDownloadRequest(wants, "/data/xemu/covers");
}

static bool SetupRecheck(int games); // First run, below

//
// The startup animation (display.ui.boot_logo): out of the dark a green
// glow wakes in the middle, trails of light spiral in to it, and in a flash
// the XPSemu emblem is there, glowing, with the name under it; a light runs
// across the emblem, then it all fades and the dashboard comes up. Drawn
// over everything; the startup sound plays with it, the music after it.
//

#include "boot-logo.h"

static const float kBootLen = 4.7f;
static XemuTexture g_boot_logo;

static void BootLogoLoad()
{
    if (g_boot_logo) {
        return;
    }
    int w, h, channels;
    unsigned char *data = stbi_load_from_memory(
        kBootLogoPng, sizeof(kBootLogoPng), &w, &h, &channels, 4);
    if (data) {
        g_boot_logo = xemu_vk_texture_create(data, w, h, 4);
        stbi_image_free(data);
    }
}

void DashboardScene::DrawBoot(float s)
{
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImVec2 c(size.x / 2, size.y * 0.44f);
    float t = m_boot_t;
    auto clamp01 = [](float x) { return std::clamp(x, 0.f, 1.f); };
    auto ease = [&](float x) {
        x = clamp01(x);
        return 1 - (1 - x) * (1 - x) * (1 - x);
    };
    float out = 1 - clamp01((t - 4.2f) / 0.5f); // Everything fades at the end

    dl->AddRectFilled(ImVec2(0, 0), size, Rgba(0, 0, 0, 1));

    // The glow waking in the middle, breathing, strongest at the flash.
    float wake = clamp01((t - 0.3f) / 1.7f);
    float glow = wake * wake * (t < 2.0f ? 1 : 1 - clamp01((t - 2.0f) / 1.2f) * 0.6f);
    float breathe = 0.85f + 0.15f * sinf(t * 7);
    for (int i = 6; i >= 1; i--) {
        float r = (20 + i * 22 * (0.6f + wake)) * s * breathe;
        dl->AddCircleFilled(c, r, Rgba(90, 220, 40, out * glow * 0.07f), 48);
    }
    dl->AddCircleFilled(c, 10 * s * (0.5f + wake) * breathe,
                        Rgba(230, 255, 190, out * glow * 0.9f), 24);

    // Trails of light spiralling in from all round, meeting at 2 s.
    if (t > 0.6f && t < 2.15f) {
        const int trails = 9, points = 22;
        float reach = size.y * 0.62f;
        for (int i = 0; i < trails; i++) {
            float phase = i * 2 * (float)M_PI / trails;
            ImVec2 prev;
            for (int k = points; k >= 0; k--) {
                float p = clamp01((t - 0.6f) / 1.4f - k * 0.018f);
                float pe = p * p * (3 - 2 * p);
                float r = reach * powf(1 - pe, 1.2f);
                float th = phase + pe * 2.6f * (float)M_PI;
                ImVec2 pt(c.x + cosf(th) * r, c.y + sinf(th) * r * 0.82f);
                if (k < points) {
                    float head = 1 - (float)k / points; // 1 at the head
                    float fade = clamp01((t - 0.6f) / 0.3f) *
                                 (1 - clamp01((t - 1.95f) / 0.2f));
                    dl->AddLine(prev, pt,
                                k < 2 ? Rgba(235, 255, 200, fade) :
                                        Rgba(110, 230, 50, fade * head * 0.8f),
                                (1.5f + 4.5f * head) * s);
                }
                prev = pt;
            }
        }
    }

    // The flash, and a ring of light running out from it.
    if (t >= 2.0f && t < 2.8f) {
        float f = 1 - ease((t - 2.0f) / 0.45f);
        dl->AddRectFilled(ImVec2(0, 0), size, Rgba(200, 255, 150, f * 0.75f));
        float rp = ease((t - 2.0f) / 0.8f);
        dl->AddCircle(c, size.y * 0.7f * rp, Rgba(170, 245, 90, (1 - rp) * 0.8f),
                      96, 6 * s * (1 - rp) + 1);
    }

    // The emblem: there in the flash, settling from a little bigger, with
    // its glow pulsing behind it.
    if (t >= 2.0f && g_boot_logo) {
        float in = ease((t - 2.0f) / 0.7f);
        float ea = clamp01((t - 2.0f) / 0.25f) * out;
        float side = size.y * 0.34f * (1.25f - 0.25f * in);
        float pulse = 0.75f + 0.25f * sinf(t * 3);
        for (int i = 2; i >= 1; i--) {
            float g = side * (1 + 0.18f * i);
            dl->AddImage((ImTextureID)g_boot_logo,
                         ImVec2(c.x - g / 2, c.y - g / 2),
                         ImVec2(c.x + g / 2, c.y + g / 2), ImVec2(0, 0),
                         ImVec2(1, 1), Rgba(140, 255, 90, ea * 0.18f * pulse / i));
        }
        ImVec2 e0(c.x - side / 2, c.y - side / 2), e1(c.x + side / 2, c.y + side / 2);
        dl->AddImage((ImTextureID)g_boot_logo, e0, e1, ImVec2(0, 0), ImVec2(1, 1),
                     White(ea));
        // A light across it (only where the emblem is: its own picture,
        // a slanted slice of it, in white).
        float q = (t - 3.1f) / 0.8f;
        if (q > 0 && q < 1) {
            float band = 0.18f, slant = 0.35f;
            float u0 = -band - slant + q * (1 + 2 * band + slant);
            ImVec2 uv[4] = { ImVec2(u0 + slant, 0), ImVec2(u0 + slant + band, 0),
                             ImVec2(u0 + band, 1), ImVec2(u0, 1) };
            ImVec2 pt[4];
            for (int k = 0; k < 4; k++) {
                pt[k] = ImVec2(e0.x + uv[k].x * side, e0.y + uv[k].y * side);
            }
            dl->PushClipRect(e0, e1, true);
            dl->AddImageQuad((ImTextureID)g_boot_logo, pt[0], pt[1], pt[2],
                             pt[3], uv[0], uv[1], uv[2], uv[3],
                             White(ea * 0.55f * sinf(q * (float)M_PI)));
            dl->PopClipRect();
        }

        // Sparkles round it.
        for (int i = 0; i < 12; i++) {
            float ang = i * 2.39996f, dist = side * (0.55f + 0.12f * (i % 3));
            ImVec2 sp(c.x + cosf(ang + t * 0.3f) * dist,
                      c.y + sinf(ang + t * 0.3f) * dist * 0.85f);
            float tw = clamp01(sinf(t * 4 + i * 1.7f));
            float r = (2 + 3 * tw) * s;
            ImU32 col = White(out * clamp01((t - 2.6f) / 0.4f) * tw * 0.8f);
            dl->AddLine(ImVec2(sp.x - r, sp.y), ImVec2(sp.x + r, sp.y), col, 1.5f * s);
            dl->AddLine(ImVec2(sp.x, sp.y - r), ImVec2(sp.x, sp.y + r), col, 1.5f * s);
        }
    }

    // The name, rising into place under it, letters spaced out.
    if (t >= 2.6f) {
        ImFont *font = g_font_mgr.m_menu_font_medium;
        float na = ease((t - 2.6f) / 0.6f) * out;
        float fs = 96 * s, spacing = 14 * s;
        const char *name = "XPSemu";
        float w = 0;
        for (const char *ch = name; *ch; ch++) {
            char one[2] = { *ch, 0 };
            w += TextSize(font, fs, one).x + spacing;
        }
        w -= spacing;
        float x = c.x - w / 2;
        float y = c.y + size.y * 0.21f + (1 - na) * 24 * s;
        for (const char *ch = name; *ch; ch++) {
            char one[2] = { *ch, 0 };
            for (int g = 0; g < 4; g++) { // A soft green glow
                float ox = (g & 1 ? 3 : -3) * s, oy = (g & 2 ? 3 : -3) * s;
                Text(dl, font, fs, ImVec2(x + ox, y + oy),
                     Rgba(110, 230, 50, na * 0.18f), one);
            }
            Text(dl, font, fs, ImVec2(x, y), White(na), one);
            x += TextSize(font, fs, one).x + spacing;
        }
    }
}

//
// Home screen shortcuts: a game of its own on the PS5's home screen. Game
// settings > Home screen shortcut makes /data/homebrew/PPSA98nnn/ — a copy
// of XPSemu (its eboot, module, backgrounds) with its own title ID, the
// game's name and an icon made from its cover, and xpsemu-game.txt naming
// the disc — and puts the ID in /data/whitelist.txt (the jailbreak helper
// asks for it). ShadowMountPlus installs the folder within seconds, as it
// installed XPSemu. Opened from the home screen it's XPSemu, which sees
// its title ID isn't its own, plays the startup animation and starts the
// game straight away.
//

#include "boot-logo.h"

extern "C" {
typedef struct XpsAppInfo {
    uint32_t app_id;
    uint64_t unknown1;
    char title_id[14];
    char unknown2[0x3c];
} XpsAppInfo;
int sceKernelGetAppInfo(pid_t pid, XpsAppInfo *info);
int xemu_ps5_import_ok(const char *name);
}

static const char *const kMainTitle = "PPSA97358";
static const char *const kHomebrew = "/data/homebrew";

// The title ID this copy of XPSemu runs as ("" if unknown).
static std::string OwnTitleId()
{
    if (!xemu_ps5_import_ok("sceKernelGetAppInfo")) {
        return "";
    }
    XpsAppInfo info;
    memset(&info, 0, sizeof(info));
    if (sceKernelGetAppInfo(getpid(), &info) != 0) {
        return "";
    }
    return std::string(info.title_id, strnlen(info.title_id, sizeof(info.title_id)));
}

static std::string ReadSmallFile(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    return text;
}

// The disc this copy is a shortcut to, or "" (the real XPSemu).
static std::string ShortcutGame()
{
    std::string id = OwnTitleId();
    fprintf(stderr, "XPSemu: running as %s\n", id.empty() ? "(unknown)" : id.c_str());
    if (id.empty() || id == kMainTitle) {
        return "";
    }
    return ReadSmallFile(std::string(kHomebrew) + "/" + id + "/xpsemu-game.txt");
}

// Shortcuts from the first version (XPSGnnnnn: not a title ID the PS5
// installs) are taken away; they never worked.
static void RemoveOldShortcuts()
{
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(kHomebrew, ec)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("XPSG", 0) == 0 &&
            std::filesystem::exists(entry.path() / "xpsemu-game.txt", ec)) {
            std::filesystem::remove_all(entry.path(), ec);
            fprintf(stderr, "XPSemu: removed old shortcut %s\n", name.c_str());
        }
    }
}

// The shortcut folder for a disc, if there is one.
static std::string ShortcutDirFor(const std::string &iso)
{
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(kHomebrew, ec)) {
        std::string name = entry.path().filename().string();
        if (name != kMainTitle &&
            ReadSmallFile(entry.path().string() + "/xpsemu-game.txt") == iso) {
            return entry.path().string();
        }
    }
    return "";
}

static bool CopyWhole(const std::string &from, const std::string &to)
{
    FILE *in = fopen(from.c_str(), "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(to.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    std::vector<char> buf(1 << 20);
    size_t n;
    bool ok = true;
    while ((n = fread(buf.data(), 1, buf.size(), in)) > 0) {
        ok = ok && fwrite(buf.data(), 1, n, out) == n;
    }
    fclose(in);
    ok = fclose(out) == 0 && ok;
    return ok;
}

static void ReplaceAll(std::string &s, const std::string &from, const std::string &to)
{
    for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += to.size()) {
        s.replace(at, from.size(), to);
    }
}

// The icon (512x512): the cover blurred and dark behind, the cover itself in
// front, the XPSemu emblem in a corner. false: no cover to make it from.
static bool MakeIcon(const std::string &cover_file, std::vector<uint8_t> *png)
{
    const int N = 512;
    std::vector<uint8_t> cover;
    int cw, ch;
    if (cover_file.empty() || !DecodePicture(cover_file, N, &cover, &cw, &ch)) {
        return false;
    }
    auto sample = [&](float u, float v, int c) { // Bilinear, clamped
        float x = std::clamp(u * cw - 0.5f, 0.f, cw - 1.f);
        float y = std::clamp(v * ch - 0.5f, 0.f, ch - 1.f);
        int x0 = (int)x, y0 = (int)y, x1 = std::min(x0 + 1, cw - 1),
            y1 = std::min(y0 + 1, ch - 1);
        float fx = x - x0, fy = y - y0;
        auto px = [&](int xx, int yy) { return cover[((size_t)yy * cw + xx) * 4 + c]; };
        return (px(x0, y0) * (1 - fx) + px(x1, y0) * fx) * (1 - fy) +
               (px(x0, y1) * (1 - fx) + px(x1, y1) * fx) * fy;
    };
    std::vector<float> img((size_t)N * N * 3);
    // The cover filling the square: as wide as the icon, its middle.
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            float u = (x + 0.5f) / N;
            float v = 0.5f + ((y + 0.5f) / N - 0.5f) * cw / (float)ch;
            for (int c = 0; c < 3; c++) {
                img[((size_t)y * N + x) * 3 + c] = sample(u, v, c);
            }
        }
    }
    // The emblem, bottom right.
    int ew, eh, en;
    unsigned char *emblem = stbi_load_from_memory(kBootLogoPng, sizeof(kBootLogoPng),
                                                  &ew, &eh, &en, 4);
    if (emblem) {
        const int E = 96, ex0 = N - E - 6, ey0 = N - E - 6;
        for (int y = 0; y < E; y++) {
            for (int x = 0; x < E; x++) {
                const unsigned char *q = emblem + ((size_t)(y * eh / E) * ew + x * ew / E) * 4;
                float al = q[3] / 255.0f;
                float *p = &img[((size_t)(ey0 + y) * N + ex0 + x) * 3];
                for (int c = 0; c < 3; c++) p[c] = p[c] * (1 - al) + q[c] * al;
            }
        }
        stbi_image_free(emblem);
    }
    std::vector<uint8_t> rgb(img.size());
    for (size_t i = 0; i < img.size(); i++) {
        rgb[i] = (uint8_t)std::clamp(img[i], 0.f, 255.f);
    }
    fpng::fpng_init(); // Once is enough; again is harmless
    return fpng::fpng_encode_image_to_memory(rgb.data(), N, N, 3, *png);
}

// ShadowMountPlus (it installs the shortcuts) asked to look through
// /data/homebrew now: through its local API, it waits until no app runs, so
// the tile appears the moment XPSemu closes, whichever way. Quick to give up
// (it may not be running).
static bool g_shortcuts_changed;

static void ShadowMountScan()
{
    g_shortcuts_changed = true;
    std::string body;
    bool ok = CoverHttpPost("http://127.0.0.1:10101/api/v1/scan",
                            "{\"reset_attempts\":false}", &body);
    fprintf(stderr, "XPSemu: ShadowMountPlus scan %s: %s\n",
            ok ? "asked" : "not reachable", body.substr(0, 120).c_str());
}

struct ShortcutJob {
    bool remove;
    std::string iso, name, cover_file, dir;
    std::vector<std::string> names; // For its background art
};
static std::mutex g_shortcut_lock;
static std::string g_shortcut_message; // For the UI to show
static bool g_shortcut_busy;

static bool WriteWhole(const std::string &path, const void *data, size_t size)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "XPSemu: shortcut: can't write %s (%d)\n", path.c_str(), errno);
        return false;
    }
    bool ok = fwrite(data, 1, size, f) == size;
    ok = fflush(f) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    return ok;
}

static long long FileSize(const std::string &path)
{
    std::error_code ec;
    auto n = std::filesystem::file_size(path, ec);
    return ec ? -1 : (long long)n;
}

// A file's bytes (at most max), or "".
static std::string ReadBinary(const std::string &path, size_t max = 64 << 20)
{
    std::string data;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return data;
    }
    char buf[65536];
    size_t n;
    while (data.size() < max &&
           (n = fread(buf, 1, std::min(sizeof(buf), max - data.size()), f)) > 0) {
        data.append(buf, n);
    }
    fclose(f);
    return data;
}

// The shortcut's home-screen backgrounds (sce_sys/pic0.dds, pic1.dds): the
// game's own art from the LaunchBox Games Database (kept in
// /data/xemu/backgrounds), filling 3840x2160, in BC7 like XPSemu's own (its
// header reused). "" if made, else why not (XPSemu's stay).
static std::string MakeBackground(const ShortcutJob &job, const std::string &stage)
{
    std::string url = CoverLaunchboxArt(job.names);
    if (url.empty()) {
        return "no art for it in LaunchBox";
    }
    std::string dir = std::string(xemu_settings_get_base_path()) + "backgrounds";
    std::string file = dir + "/" + url.substr(url.rfind('/') + 1);
    std::string body = ReadBinary(file);
    if (body.empty()) {
        if (!CoverHttpGet(url, &body) || body.size() < 1024) {
            return "couldn't download " + url;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        WriteWhole(file, body.data(), body.size());
    }
    int w, h, n;
    unsigned char *img = stbi_load_from_memory((const unsigned char *)body.data(),
                                               (int)body.size(), &w, &h, &n, 3);
    if (!img) {
        return "can't read " + file;
    }
    const int W = 3840, H = 2160;
    std::string header = ReadBinary(stage + "/sce_sys/pic0.dds", 148);
    if (header.size() != 148 || header.compare(0, 4, "DDS ") != 0) {
        stbi_image_free(img);
        return "XPSemu's pic0.dds isn't the expected kind";
    }
    std::vector<uint8_t> dds(148 + (size_t)W * H);
    memcpy(dds.data(), header.data(), 148);
    // Filling the screen (cropped, not stretched); a little darker at the
    // bottom left, where the PS5 writes the name.
    float k = std::max((float)W / w, (float)H / h);
    float ox = (w - W / k) / 2, oy = (h - H / k) / 2;
    for (int by = 0; by < H / 4; by++) {
        for (int bx = 0; bx < W / 4; bx++) {
            uint8_t px[16][3];
            for (int i = 0; i < 16; i++) {
                int X = bx * 4 + i % 4, Y = by * 4 + i / 4;
                float sx = std::clamp(ox + (X + 0.5f) / k - 0.5f, 0.f, w - 1.f);
                float sy = std::clamp(oy + (Y + 0.5f) / k - 0.5f, 0.f, h - 1.f);
                int x0 = (int)sx, y0 = (int)sy, x1 = std::min(x0 + 1, w - 1),
                    y1 = std::min(y0 + 1, h - 1);
                float fx = sx - x0, fy = sy - y0;
                float shade = 1 - 0.35f * std::max(0.f, 1 - X / (0.55f * W)) *
                                      std::clamp((Y - 0.35f * H) / (0.4f * H), 0.f, 1.f);
                for (int c = 0; c < 3; c++) {
                    auto at = [&](int xx, int yy) { return img[((size_t)yy * w + xx) * 3 + c]; };
                    float v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) +
                              (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                    px[i][c] = (uint8_t)std::clamp(v * shade + 0.5f, 0.f, 255.f);
                }
            }
            Bc7EncodeBlock(px, &dds[148 + ((size_t)by * (W / 4) + bx) * 16]);
        }
    }
    stbi_image_free(img);
    if (!WriteWhole(stage + "/sce_sys/pic0.dds", dds.data(), dds.size()) ||
        !WriteWhole(stage + "/sce_sys/pic1.dds", dds.data(), dds.size())) {
        return "can't write the backgrounds";
    }
    fprintf(stderr, "XPSemu: shortcut background from %s (%dx%d)\n",
            url.c_str(), w, h);
    return "";
}

static std::string MakeShortcut(const ShortcutJob &job)
{
    std::error_code ec;
    if (job.remove) {
        std::filesystem::remove_all(job.dir, ec);
        return ec ? "Couldn't remove the shortcut: " + ec.message() :
                    job.name + ": shortcut removed (delete its tile from the "
                    "home screen too)";
    }
    // The next free PPSA98nnn: a PS5-style ID, in a range no game uses.
    std::string id;
    for (int n = 1; n < 1000 && id.empty(); n++) {
        char cand[16];
        snprintf(cand, sizeof(cand), "PPSA98%03d", n);
        if (!std::filesystem::exists(std::string(kHomebrew) + "/" + cand, ec)) {
            id = cand;
        }
    }
    std::string src = std::string(kHomebrew) + "/" + kMainTitle;
    std::string dir = std::string(kHomebrew) + "/" + id;
    // Made whole somewhere ShadowMountPlus doesn't look, then moved in at
    // once: it never sees a folder half written.
    std::string stage = std::string(xemu_settings_get_base_path()) +
                        "shortcut-tmp/" + id;
    std::filesystem::remove_all(stage, ec);
    std::filesystem::create_directories(stage + "/sce_sys", ec);
    std::filesystem::create_directories(stage + "/sce_module", ec);
    auto fail = [&](const std::string &why) {
        fprintf(stderr, "XPSemu: shortcut %s failed: %s\n", id.c_str(), why.c_str());
        std::filesystem::remove_all(stage, ec);
        return "Couldn't make the shortcut: " + why;
    };
    if (ec) {
        return fail("can't make " + stage);
    }
    for (const char *f : { "eboot.bin", "imports.txt", "sce_module/libc.prx",
                           "sce_sys/pic0.dds", "sce_sys/pic1.dds" }) {
        if (!CopyWhole(src + "/" + f, stage + "/" + f)) {
            return fail(std::string("can't copy ") + src + "/" + f);
        }
        fprintf(stderr, "XPSemu: shortcut %s: %s (%lld bytes)\n", id.c_str(), f,
                FileSize(stage + "/" + f));
    }
    // param.json: XPSemu's, with the shortcut's ID and the game's name.
    std::string param = ReadSmallFile(src + "/sce_sys/param.json");
    if (param.find(kMainTitle) == std::string::npos) {
        return fail("can't read " + src + "/sce_sys/param.json (" +
                    std::to_string(param.size()) + " bytes)");
    }
    std::string name;
    for (char c : job.name) { // JSON-safe
        if (c == '"' || c == '\\') name += '\\';
        if ((unsigned char)c >= 0x20) name += c;
    }
    ReplaceAll(param, kMainTitle, id);
    // A content ID of its own too (XPSemu's would be a duplicate).
    ReplaceAll(param, "XEMUPS5000000001", "XPSEMUSC000" + id.substr(4));
    ReplaceAll(param, "\"titleName\": \"XPSemu\"", "\"titleName\": \"" + name + "\"");
    ReplaceAll(param, "\"conceptId\": \"97358\"", "\"conceptId\": \"" + id.substr(4) + "\"");
    param += "\n";
    std::string game = job.iso + "\n";
    std::vector<uint8_t> png;
    bool icon = MakeIcon(job.cover_file, &png);
    std::string no_background = MakeBackground(job, stage);
    if (!no_background.empty()) {
        fprintf(stderr, "XPSemu: shortcut %s: XPSemu's background (%s)\n",
                id.c_str(), no_background.c_str());
    }
    if (!WriteWhole(stage + "/xpsemu-game.txt", game.data(), game.size()) ||
        !(icon ? WriteWhole(stage + "/sce_sys/icon0.png", png.data(), png.size()) :
                 CopyWhole(src + "/sce_sys/icon0.png", stage + "/sce_sys/icon0.png")) ||
        !WriteWhole(stage + "/sce_sys/param.json", param.data(), param.size())) {
        return fail("can't write the shortcut's files");
    }
    // Read back: ShadowMountPlus needs its titleId.
    std::string check = ReadSmallFile(stage + "/sce_sys/param.json");
    if (check.find("\"titleId\": \"" + id + "\"") == std::string::npos) {
        return fail("param.json didn't write (" + std::to_string(check.size()) +
                    " bytes)");
    }
    fprintf(stderr, "XPSemu: shortcut %s: param.json %zu bytes, icon %s\n",
            id.c_str(), check.size(), icon ? "from the cover" : "XPSemu's");
    // The jailbreak helper's whitelist, before the app can run.
    std::string list = ReadSmallFile("/data/whitelist.txt");
    if (list.find(id) == std::string::npos) {
        std::string line = (list.empty() ? "" : "\n") + id + "\n";
        FILE *f = fopen("/data/whitelist.txt", "a");
        if (!f || fwrite(line.data(), 1, line.size(), f) != line.size()) {
            fprintf(stderr, "XPSemu: shortcut %s: whitelist not written (%d)\n",
                    id.c_str(), errno);
        }
        if (f) {
            fclose(f);
        }
    }
    // Runnable, as an app installed over FTP is (copies are made 0666).
    for (const auto &entry : std::filesystem::recursive_directory_iterator(stage, ec)) {
        chmod(entry.path().c_str(), 0777);
    }
    chmod(stage.c_str(), 0777);
    std::filesystem::rename(stage, dir, ec);
    if (ec) {
        return fail("can't move it into " + dir + ": " + ec.message());
    }
    fprintf(stderr, "XPSemu: shortcut %s for %s\n", id.c_str(), job.iso.c_str());
    return job.name + ": shortcut made (" + id + ")" +
           (no_background.empty() ? " with its own background" : "") +
           ", on the home screen in a few seconds";
}

void DashboardScene::ToggleShortcut(const Game &game)
{
    {
        std::lock_guard<std::mutex> guard(g_shortcut_lock);
        if (g_shortcut_busy) {
            return;
        }
        g_shortcut_busy = true;
    }
    ShortcutJob job;
    job.dir = ShortcutDirFor(game.path);
    job.remove = !job.dir.empty();
    job.iso = game.path;
    job.name = game.name;
    job.names = { game.name, Art(game.path).info.title, Art(game.path).stem };
    for (const auto &key : CoverKeys(game)) {
        auto it = g_covers.find(key);
        if (!key.empty() && it != g_covers.end()) {
            job.cover_file = it->second;
            break;
        }
    }
    xemu_queue_notification(job.remove ? "Removing the shortcut..." :
                                         "Making the shortcut...");
    std::thread([job]() {
        std::string message = MakeShortcut(job);
        ShadowMountScan(); // Installed as soon as it may (when XPSemu closes)
        std::lock_guard<std::mutex> guard(g_shortcut_lock);
        g_shortcut_message = message;
        g_shortcut_busy = false;
    }).detach();
}

void DashboardScene::Show()
{
    m_startup_fade = m_startup_show;
    m_startup_fade_elapsed = 0;
    m_startup_black = 0;
    m_startup_smooth = 0;
    m_startup_settled = false;
    if (m_startup_fade) {
        m_alpha = 0;
    }
    m_startup_show = false;
    if (!m_recent_loaded) {
        LoadRecent();
    }
    PatchesRescan();
    ScanGames();
    m_np_focus = false;
    m_back_hold = 0;
    m_idle = 0;
    m_attract = false;
    m_attract_alpha = 0;
    m_setup = SetupRecheck((int)m_games.size());
    m_setup_ok = false;
    m_visible = true;
    m_in_page = false;
    m_advanced = false;
    m_settings_cat = -1;
    m_view = false;
    m_pad_page = false;
    m_pad_capture = false;
    m_kb_open = false;
    m_all_page = false;
    m_shelf = false;
    m_gs_open = false;
    m_gs_patches = false;
    m_page_anim = 0;
    // The press that opened the dashboard mustn't also act in it.
    m_prev_buttons = g_input_mgr.CombinedButtons();
    // At startup, XPSemu's own sound (once), before the dashboard
    // fades in; otherwise the usual one.
    UiSoundPlay(m_startup_fade && !m_setup ? UI_SOUND_STARTUP : UI_SOUND_OPEN);
    // The music fades in: at startup once the startup sound is under way
    // (loaded now, in the startup stall), else straight away.
    if (m_startup_fade) {
        m_autolaunch = ShortcutGame();
        RemoveOldShortcuts();
        UiMusicPrepare();
        // The logo first (the music after it), else the music a little in.
        // Not until the Xbox's files are there: the checklist first.
        m_boot = g_config.display.ui.boot_logo && !m_setup;
        m_boot_t = 0;
        m_music_at = m_boot ? -1 : m_time + 3.5f;
        if (m_boot || m_setup) {
            BootLogoLoad(); // The checklist shows the emblem too
        }
    } else {
        UiMusicPlay(true);
    }
    // Hold the game (or the Xbox dashboard) while this one is up.
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
        m_paused_vm = true;
    }
}

void DashboardScene::Hide()
{
    m_visible = false;
    m_music_at = -1;
    UiMusicPlay(false); // Fades out, into the game
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

static void CurtainStart(const DashboardScene::Game &game, bool plain); // Below
static void CurtainFrom(ImVec2 p0, ImVec2 p1);

static void SetXboxVideoFor(bool patch_720p, bool widescreen); // Below

void DashboardScene::Launch(const Game &game, bool quiet)
{
    ActionLoadDiscFile(game.path.c_str());
    m_now_playing_path = game.path;
    GameStart(KeyOf(game)); // Its own settings, and its play time
    std::string overrides;
    const GameProfile &profile = ProfileOf(game);
    for (int o = GO_SCALE + 1; o < GO__COUNT; o++) { // Not resolution
        if (o == GO_FILTER || o == GO_DSP) {
            continue; // Main settings only
        }
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
    {
        std::string lower = patches;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        // A 720p patch that applies: a line of the log naming 720p,
        // not skipped.
        bool p720 = false;
        for (size_t at = 0, end; at < lower.size(); at = end + 1) {
            end = lower.find('\n', at);
            if (end == std::string::npos) {
                end = lower.size();
            }
            std::string line = lower.substr(at, end - at);
            if (line.find("720p") != std::string::npos &&
                line.find(" - skipped") == std::string::npos) {
                p720 = true;
            }
        }
        int aspect = profile.value[GO_ASPECT] != GAME_DEFAULT ?
                         profile.value[GO_ASPECT] : GameGlobal(GO_ASPECT);
        SetXboxVideoFor(p720, aspect == CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9);
    }
    CurtainStart(game, quiet); // Over the Xbox's boot until the game runs
    GameLogStart({ game.name, game.path, KeyOf(game), xiso.title,
                   game.title_id, game.full_disc, overrides, patches });
    if (!quiet) {
        UiSoundPlay(UI_SOUND_LAUNCH);
    }
    // Keep game boots quick after the Xbox starts the title.
    xbox_smc_set_short_animation();
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

//
// The launch screen: from Launch until the game itself runs, the game's
// cover over the Xbox's boot, with the console's sound held down. The Xbox's
// startup animation plays only when XPSemu starts.
//

extern "C" float g_xemu_apu_ui_gain; // hw/xbox/mcpx/apu/monitor.c

// A card in 3D: the corners (tl, tr, br, bl) of a w x h card around c,
// turned by ry about its upright axis and rx about its level one, then
// rolled, in perspective (focal length f). front: its face is towards us.
struct Card {
    ImVec2 p[4];
    bool front;
};

static Card Card3D(ImVec2 c, float w, float h, float ry, float rx,
                   float roll, float f)
{
    Card card;
    const float lx[4] = { -w / 2, w / 2, w / 2, -w / 2 };
    const float ly[4] = { -h / 2, -h / 2, h / 2, h / 2 };
    float cy = cosf(ry), sy = sinf(ry), cx = cosf(rx), sx = sinf(rx);
    float cr = cosf(roll), sr = sinf(roll);
    for (int i = 0; i < 4; i++) {
        float x = lx[i] * cy, z = lx[i] * sy;
        float y = ly[i] * cx;
        z += ly[i] * sx;
        float k = f / std::max(f + z, f * 0.2f);
        float rx2 = x * cr - y * sr, ry2 = x * sr + y * cr;
        card.p[i] = ImVec2(c.x + rx2 * k, c.y + ry2 * k);
    }
    card.front = cy * cx > 0;
    return card;
}

static XemuTexture DrawCase(const DashboardScene::Game &game, ImVec2 tl,
                            ImVec2 tr, ImVec2 br, ImVec2 bl, float a,
                            float dim, ImDrawList *dl = nullptr); // Below

// A glint of light crossing the box p0..p1 at q (0..1).
static void Glint(ImDrawList *dl, ImVec2 p0, ImVec2 p1, float q, float a)
{
    float bw = (p1.x - p0.x) * 0.28f, sh = p1.y - p0.y;
    float gx = p0.x - bw - sh * 0.35f + q * (p1.x - p0.x + bw * 2 + sh * 0.35f);
    dl->PushClipRect(p0, p1, true);
    dl->AddQuadFilled(ImVec2(gx + sh * 0.35f, p0.y),
                      ImVec2(gx + sh * 0.35f + bw, p0.y), ImVec2(gx + bw, p1.y),
                      ImVec2(gx, p1.y), White(a * sinf(q * (float)M_PI)));
    dl->PopClipRect();
}

static struct {
    bool active = false, lifting = false, seen_other = false;
    double started = 0, matched = 0, next_check = 0;
    float alpha = 0;
    uint32_t title_id = 0;
    std::string name, path;
    DashboardScene::Game game;
    bool has_from = false; // Launched from the Games row: the cover's box
    ImVec2 from0, from1;
    bool plain = false; // From a shortcut: just black, no case
} g_curtain;

static bool g_curtain_from_set;
static ImVec2 g_curtain_from0, g_curtain_from1;

// Where the launched game's cover is on screen, for the next CurtainStart.
static void CurtainFrom(ImVec2 p0, ImVec2 p1)
{
    g_curtain_from_set = p1.x > p0.x && p1.y > p0.y;
    g_curtain_from0 = p0;
    g_curtain_from1 = p1;
}

static void CurtainStart(const DashboardScene::Game &game, bool plain)
{
    g_curtain.active = true;
    g_curtain.plain = plain;
    g_curtain.lifting = false;
    g_curtain.seen_other = false;
    g_curtain.started = ImGui::GetTime();
    g_curtain.matched = 0;
    g_curtain.next_check = 0;
    g_curtain.alpha = 1;
    g_curtain.title_id = game.title_id;
    g_curtain.name = game.name;
    g_curtain.path = game.path;
    g_curtain.game = game;
    g_curtain.has_from = g_curtain_from_set;
    g_curtain.from0 = g_curtain_from0;
    g_curtain.from1 = g_curtain_from1;
    g_curtain_from_set = false;
    g_xemu_apu_ui_gain = 0.0f;
}

static void CurtainStop()
{
    g_curtain.active = false;
    g_xemu_apu_ui_gain = 1.0f;
}

void DrawLaunchCurtain()
{
    if (!g_curtain.active) {
        return;
    }
    double now = ImGui::GetTime();
    float dt = std::min(ImGui::GetIO().DeltaTime, 1 / 30.0f);

    // Has the game started? Its XBE is what runs. Right after the reset the
    // Xbox runs something else (or nothing) first, which tells a relaunch of
    // the same game from the old run still in memory.
    if (!g_curtain.lifting && now >= g_curtain.next_check) {
        g_curtain.next_check = now + 0.1;
        struct xbe *xbe = xemu_get_xbe_info();
        uint32_t running = xbe && xbe->cert ? xbe->cert->m_titleid : 0;
        if (running != g_curtain.title_id) {
            g_curtain.seen_other = true;
        } else if (g_curtain.seen_other && !g_curtain.matched) {
            g_curtain.matched = now;
        }
        bool ready = g_curtain.matched && now - g_curtain.matched > 0.4;
        double limit = g_curtain.title_id ? 15 : 5; // Never stuck over it
        if (ready || now - g_curtain.started > limit) {
            g_curtain.lifting = true;
        }
    }
    if (g_curtain.lifting) {
        g_curtain.alpha -= dt / 0.6f;
        if (g_curtain.alpha <= 0) {
            CurtainStop();
            return;
        }
        g_xemu_apu_ui_gain = 1 - g_curtain.alpha;
    }

    // Under the dashboard (fading out after Launch), over the game's picture:
    // the cover flies from its place in the row to the middle, swinging
    // round on the way, and grows to fill the screen over its own picture,
    // blurred. A glint crossing it now and then says it's loading; when the
    // game runs it flies on towards us as it fades.
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float s = size.y / 1080.0f;
    float a = g_curtain.alpha;
    float t = (float)(now - g_curtain.started);
    if (g_curtain.plain) { // Black from the logo until the game shows
        dl->AddRectFilled(ImVec2(0, 0), size, Rgba(0, 0, 0, a));
        return;
    }
    XemuTexture cover = Art(g_curtain.path).cover;

    dl->AddRectFilled(ImVec2(0, 0), size, Rgba(0, 8, 0, a));
    float bg = std::clamp(t / 0.6f, 0.f, 1.f);
    if (cover) {
        DrawBlurredFill(dl, cover, false, size, 40 * s, t, a * 0.6f * bg);
    }
    float ga = a * (cover ? 0.7f : 1.0f);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, Rgba(4, 40, 4, ga),
                                Rgba(2, 26, 2, ga), Rgba(0, 12, 0, ga),
                                Rgba(2, 24, 2, ga));

    float hh = size.y * 0.74f, hw = hh * 0.71f;
    ImVec2 hc(size.x / 2, size.y * 0.45f);
    float p = std::clamp(t / 0.7f, 0.f, 1.f);
    float e = 1 - (1 - p) * (1 - p) * (1 - p);
    ImVec2 c0 = hc;
    float w0 = hw * 0.55f, h0 = hh * 0.55f;
    if (g_curtain.has_from) {
        c0 = ImVec2((g_curtain.from0.x + g_curtain.from1.x) / 2,
                    (g_curtain.from0.y + g_curtain.from1.y) / 2);
        w0 = g_curtain.from1.x - g_curtain.from0.x;
        h0 = g_curtain.from1.y - g_curtain.from0.y;
    }
    ImVec2 c(c0.x + (hc.x - c0.x) * e, c0.y + (hc.y - c0.y) * e);
    float zoom = 1 + 0.035f * std::clamp((t - 0.7f) / 8, 0.f, 1.f) +
                 (g_curtain.lifting ? (1 - a) * 0.35f : 0);
    float w = (w0 + (hw - w0) * e) * zoom, h = (h0 + (hh - h0) * e) * zoom;
    float ry = sinf(e * (float)M_PI) * 0.45f * (c0.x < hc.x ? -1 : 1);
    float ha = a * (g_curtain.has_from ? 1 : std::min(e * 1.5f, 1.f));
    Card card = Card3D(c, w, h, ry, 0, 0, 2.6f * hh);

    // A soft light behind it.
    for (int i = 4; i >= 1; i--) {
        float g = i * 22 * s;
        dl->AddRectFilled(ImVec2(c.x - w / 2 - g, c.y - h / 2 - g),
                          ImVec2(c.x + w / 2 + g, c.y + h / 2 + g),
                          Lime(ha * 0.03f), 30 * s + g);
    }
    DrawCase(g_curtain.game, card.p[0], card.p[1], card.p[2], card.p[3], ha,
             1, dl);
    if (p >= 1 && !g_curtain.lifting) {
        float q = fmodf(t - 0.7f, 2.2f) / 0.9f;
        if (q < 1) {
            Glint(dl, ImVec2(c.x - w / 2, c.y - h / 2),
                  ImVec2(c.x + w / 2, c.y + h / 2), q, ha * 0.2f);
        }
    }

    ImFont *font = g_font_mgr.m_menu_font_medium;
    float fs = 46 * s;
    ImVec2 ns = TextSize(font, fs, g_curtain.name.c_str());
    float na = ha * std::clamp((t - 0.35f) / 0.35f, 0.f, 1.f);
    Text(dl, font, fs, ImVec2(hc.x - ns.x / 2, hc.y + hh / 2 + 24 * s),
         White(na), g_curtain.name.c_str());
}

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
        StopGame("back to the Xbox dashboard");
    }
    UiSoundPlay(UI_SOUND_LAUNCH);
    m_can_return = true;
    m_paused_vm = true; // Hide starts it (it may never have run)
    Hide();
}

// Ends the running game: the disc comes out, the Xbox restarts into its
// own dashboard (started when the dashboard hides), Now Playing goes.
extern "C" int sceSystemServiceLoadExec(const char *path,
                                        const char *const *argv);
extern "C" int xemu_ps5_import_ok(const char *name);

// Restarting XPSemu (from its own start: the boot logo and all, as if
// opened again) or leaving it, as VLC-PS5 does: the system loads our eboot
// again, or "exit". A running game ends properly first (its play time and
// log saved).
void DashboardScene::QuitApp(bool restart)
{
    if (m_game_started) {
        GameStop();
        PatchesStop();
        WatchStop();
        GameLogStop(restart ? "XPSemu restarted" : "XPSemu exited");
    }
    xemu_settings_save();
    if (g_shortcuts_changed) {
        ShadowMountScan(); // Again, so nothing waits for its next round
    }
    fprintf(stderr, "XPSemu: %s\n", restart ? "restarting" : "exiting");
    fflush(NULL);
    if (!xemu_ps5_import_ok("sceSystemServiceLoadExec")) {
        fprintf(stderr, "XPSemu: no sceSystemServiceLoadExec\n");
    } else if (restart) {
        for (const char *path : { "/app0/eboot.bin",
                                  "/data/homebrew/PPSA97358/eboot.bin" }) {
            int rc = sceSystemServiceLoadExec(path, NULL);
            fprintf(stderr, "XPSemu: restart via %s refused (%#x)\n", path, rc);
        }
    } else {
        sceSystemServiceLoadExec("exit", NULL);
        _exit(0);
    }
    xemu_queue_notification(restart ?
        "Couldn't restart XPSemu: close it and open it again" :
        "Couldn't exit XPSemu: close it from the PS5's menu");
}

void DashboardScene::StopGame(const char *why)
{
    const char *dvd = g_config.sys.files.dvd_path;
    if (dvd && dvd[0]) {
        ActionEjectDisc();
    }
    GameStop();
    PatchesStop();
    WatchStop();
    CurtainStop();
    GameLogStop(why);
    xbox_smc_set_short_animation();
    ActionReset();
    m_game_started = false;
    m_np_focus = false;
    m_paused_vm = true; // Hide starts it
}

// The Settings page's rows. Most are game options (a game can have its
// own value); Menu sounds and Advanced aren't.
enum SettingRow {
    SR_SCALE,
    SR_ASPECT,
    SR_FIT,
    SR_FILTER,
    SR_SIDES,
    SR_VOLUME,
    SR_SOUNDS,
    SR_MUSIC,
    SR_OVERLAY,
    SR_COVERS,
    SR_BOOT,
    SR_XBOX_BOOT,
    SR__COUNT
};
static const int kSettingOptions[SR__COUNT] = {
    GO_SCALE,  GO_ASPECT, GO_FIT, GO_FILTER, -1,
    GO_VOLUME, -1,        -1,     GO_OVERLAY, -1, -1, -1,
};

// Settings comes in categories, each a page of its rows; Advanced is its
// own page (DrawAdvanced).
enum { CAT_VIDEO, CAT_SOUND, CAT_INTERFACE, CAT_CONTROLLER, CAT_PATCHES,
       CAT_ADVANCED, CAT__COUNT };
static const struct {
    const char *name, *help;
    int rows[6], count;
} kCategories[CAT__COUNT] = {
    { "Video", "Resolution, screen shape, fit, smoothing and the side art.",
      { SR_SCALE, SR_ASPECT, SR_FIT, SR_FILTER, SR_SIDES }, 5 },
    { "Sound", "How loud the games are, the menu sounds and the music.",
      { SR_VOLUME, SR_SOUNDS, SR_MUSIC }, 3 },
    { "Interface", "The performance overlay, cover downloads, and the XPSemu "
                   "and original Xbox startups.",
      { SR_OVERLAY, SR_COVERS, SR_BOOT, SR_XBOX_BOOT }, 4 },
    { "Controller", "Your own button layout.", {}, 0 },
    { "Patches", "Every patch in use that works for one of your games, to "
                 "turn on or off.", {}, 0 },
    { "Advanced", "The Xbox's memory and video output, audio and performance.",
      {}, 0 },
};

void DashboardScene::ChangeSetting(int step)
{
    if (m_setting == SR_SOUNDS) {
        g_config.display.ui.menu_sounds = !g_config.display.ui.menu_sounds;
        xemu_settings_save();
        UiSoundPlay(UI_SOUND_CHANGE); // Heard only when turned on
        return;
    }
    if (m_setting == SR_MUSIC) {
        g_config.display.ui.menu_music = !g_config.display.ui.menu_music;
        xemu_settings_save();
        UiSoundPlay(UI_SOUND_CHANGE);
        UiMusicPlay(true); // In if just turned on, out if off
        return;
    }
    if (m_setting == SR_XBOX_BOOT) { // On: not skipped
        g_config.general.skip_boot_anim = !g_config.general.skip_boot_anim;
        xemu_settings_save();
        UiSoundPlay(UI_SOUND_CHANGE);
        return;
    }
    if (m_setting == SR_SIDES) { // Off, Green, Orange
        int now = g_config.display.ui.blades_sides ?
                      1 + std::clamp(g_config.display.ui.blades_style, 0, 1) : 0;
        now = (now + (step < 0 ? 2 : 1)) % 3;
        g_config.display.ui.blades_sides = now > 0;
        if (now) {
            g_config.display.ui.blades_style = now - 1;
        }
        xemu_settings_save();
        UiSoundPlay(UI_SOUND_CHANGE);
        return;
    }
    bool *toggle = m_setting == SR_COVERS ? &g_config.display.ui.download_covers :
                   m_setting == SR_BOOT   ? &g_config.display.ui.boot_logo :
                                            nullptr;
    if (toggle) {
        *toggle = !*toggle;
        xemu_settings_save();
        UiSoundPlay(UI_SOUND_CHANGE);
        if (m_setting == SR_COVERS && *toggle) {
            RequestMissingCovers();
        }
        return;
    }
    if (m_setting < 0 || m_setting >= SR__COUNT ||
        kSettingOptions[m_setting] < 0) {
        return;
    }
    int option = kSettingOptions[m_setting];
    UiSoundPlay(UI_SOUND_CHANGE);
    GameSetGlobal(option, GameOptionStep(option, GameGlobal(option), step,
                                         false));
}

//
// The Patch Store: Jay's Magic Patches (github.com/JayYardley/
// Xbox-Magic-Patches-by-Jay) for the game in Game settings > Game patches,
// under the ones already there. The list of them is built in
// (ps5/make-patch-catalog.py); a patch's file is downloaded when the page
// opens (into /data/xemu/patch-store, a few KB), checked against this
// copy's code exactly as the patches in use are, and Get puts it in
// /data/xemu/patches/store/<author>/, on. Getting a widescreen patch sets
// the game to 16:9, a 720p one turns on the Xbox's 720p, a 480i fix its
// 480p. 128 MB patches need more memory than XPSemu gives the Xbox.
//

#include "patch-catalog.h"

enum StoreState {
    STORE_CHECKING, STORE_WORKS, STORE_OTHER, STORE_UNSUPPORTED,
    STORE_128MB, STORE_FAILED, STORE_NOT_IN_GAME, STORE_NOT_NEEDED,
};

static std::string StoreDir()
{
    return std::string(xemu_settings_get_base_path()) + "patch-store";
}

static std::string StoreInstallDir()
{
    return std::string(xemu_settings_get_base_path()) + "patches/store";
}

// A catalog path as one file name (folders flattened).
static std::string StoreCacheFile(const char *path)
{
    std::string name = path;
    std::replace(name.begin(), name.end(), '/', '_');
    return StoreDir() + "/" + name;
}

static std::string StoreInstalledFile(const char *path)
{
    std::string p = path;
    return StoreInstallDir() + "/" + p;
}

static bool CatalogHasId(const PatchCatalogEntry &e, uint32_t id)
{
    char hex[16];
    snprintf(hex, sizeof(hex), "%08X", id);
    return strstr(e.ids, hex) != NULL;
}

static std::string UrlPath(const char *path)
{
    std::string out;
    for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
        if (isalnum(*p) || strchr("-_.~/", *p)) {
            out += (char)*p;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", *p);
            out += hex;
        }
    }
    return out;
}

// Downloads, one thread at a time; the results checked on the UI thread
// (the patch checks read the disc).
static std::mutex g_store_lock;
static std::deque<int> g_store_fetched; // Catalog indexes now in the cache
static std::set<int> g_store_failed;
static bool g_store_busy;

static void StoreFetch(std::vector<int> wanted)
{
    for (int i : wanted) {
        const PatchCatalogEntry &e = kPatchCatalog[i];
        std::string file = StoreCacheFile(e.path), body;
        std::error_code ec;
        bool ok = std::filesystem::exists(file, ec);
        if (!ok && CoverHttpGet("https://raw.githubusercontent.com/JayYardley/"
                                "Xbox-Magic-Patches-by-Jay/main/" +
                                    UrlPath(e.path),
                                &body)) {
            std::filesystem::create_directories(StoreDir(), ec);
            FILE *f = fopen((file + ".part").c_str(), "wb");
            ok = f && fwrite(body.data(), 1, body.size(), f) == body.size();
            if (f) {
                fclose(f);
            }
            ok = ok && rename((file + ".part").c_str(), file.c_str()) == 0;
        }
        std::lock_guard<std::mutex> guard(g_store_lock);
        if (ok) {
            g_store_fetched.push_back(i);
        } else {
            g_store_failed.insert(i);
        }
    }
    std::lock_guard<std::mutex> guard(g_store_lock);
    g_store_busy = false;
}

// The store's patches for the game in Game settings, and their downloads.
void DashboardScene::StoreOpen()
{
    m_store.clear();
    for (int i = 0; i < (int)(sizeof(kPatchCatalog) / sizeof(kPatchCatalog[0])); i++) {
        const PatchCatalogEntry &e = kPatchCatalog[i];
        if (!m_gs_game.title_id || !CatalogHasId(e, m_gs_game.title_id)) {
            continue;
        }
        std::error_code ec;
        if (std::filesystem::exists(StoreInstalledFile(e.path), ec)) {
            continue; // Got already: in the list above
        }
        StoreItem item;
        item.index = i;
        std::string cat = e.category;
        std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
        item.state = cat.find("128") != std::string::npos ? STORE_128MB :
                     cat.find("720") != std::string::npos ||
                             cat.find("1080") != std::string::npos ?
                                                             STORE_NOT_NEEDED :
                                                             STORE_CHECKING;
        m_store.push_back(item);
    }
    std::stable_partition(m_store.begin(), m_store.end(),
                          [](const StoreItem &item) {
                              return item.state != STORE_NOT_NEEDED;
                          });
    std::vector<int> wanted;
    for (const auto &item : m_store) {
        if (item.state == STORE_CHECKING) {
            wanted.push_back(item.index);
        }
    }
    std::lock_guard<std::mutex> guard(g_store_lock);
    g_store_failed.clear();
    if (!wanted.empty() && !g_store_busy) {
        g_store_busy = true;
        std::thread(StoreFetch, wanted).detach();
    }
}

// Each frame while a game's patches are open: downloaded ones checked.
void DashboardScene::StorePoll()
{
    std::deque<int> fetched;
    std::set<int> failed;
    {
        std::lock_guard<std::mutex> guard(g_store_lock);
        fetched.swap(g_store_fetched);
        failed = g_store_failed;
    }
    const XisoInfo &xiso = Art(m_gs_game.path).info;
    for (auto &item : m_store) {
        if (item.state != STORE_CHECKING) {
            continue;
        }
        if (failed.count(item.index)) {
            item.state = STORE_FAILED;
            continue;
        }
        std::error_code ec; // Downloaded whole (renamed in at the end)?
        if (!std::filesystem::exists(StoreCacheFile(kPatchCatalog[item.index].path),
                                     ec)) {
            continue;
        }
        PatchFile f;
        if (!PatchFileParse(StoreCacheFile(kPatchCatalog[item.index].path), &f)) {
            item.state = STORE_FAILED;
            continue;
        }
        if (!f.unsupported.empty()) {
            item.state = STORE_UNSUPPORTED;
            item.why = f.unsupported;
            continue;
        }
        item.state = STORE_NOT_IN_GAME;
        for (const auto &g : f.groups) {
            std::string problem =
                PatchGroupProblem(f, g, m_gs_game.path, xiso.xbe_offset, xiso.xbe_size);
            if (problem.empty() && !g.info) {
                item.state = STORE_WORKS;
                break;
            }
            if (problem == "Other version") {
                item.state = STORE_OTHER;
            }
        }
    }
}

// Get: into the patches in use, on; the game set up for it.
void DashboardScene::StoreGet(int row)
{
    const StoreItem &item = m_store[row];
    const PatchCatalogEntry &e = kPatchCatalog[item.index];
    if (item.state != STORE_WORKS) {
        UiSoundPlay(UI_SOUND_ERROR);
        return;
    }
    std::string to = StoreInstalledFile(e.path);
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(to).parent_path(), ec);
    std::filesystem::copy_file(StoreCacheFile(e.path), to,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        UiSoundPlay(UI_SOUND_ERROR);
        xemu_queue_notification(("Couldn't get the patch: " + ec.message()).c_str());
        return;
    }
    UiSoundPlay(UI_SOUND_SELECT);
    std::string note = std::string(e.category) + " patch by " + e.author + ": on";
    std::string cat = e.category;
    std::transform(cat.begin(), cat.end(), cat.begin(), ::tolower);
    if (cat.find("widescreen") != std::string::npos &&
        m_gs_profile.value[GO_ASPECT] == GAME_DEFAULT &&
        GameGlobal(GO_ASPECT) != CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9) {
        m_gs_profile.value[GO_ASPECT] = CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9;
        GameProfileSave(m_gs_key, m_gs_profile);
        g_profiles[m_gs_key] = m_gs_profile;
        note += ", screen shape 16:9 for this game";
    }
    if (GameAnyRunning() && m_now_playing_path == m_gs_game.path) {
        note += ". Start the game again to use it";
    }
    xemu_queue_notification(note.c_str());
    PatchesRescan();
    BuildPatchItems();
    StoreOpen();
    m_patch_row = 0;
    RefreshPatchBadges();
}

// Remove: a patch got from the store.
void DashboardScene::StoreRemove(const PatchFile &file)
{
    std::error_code ec;
    std::filesystem::remove(file.path, ec);
    UiSoundPlay(UI_SOUND_BACK);
    xemu_queue_notification(("Removed: " + file.file_name).c_str());
    PatchesRescan();
    BuildPatchItems();
    StoreOpen();
    m_patch_row = 0;
    RefreshPatchBadges();
}

static bool FromStore(const PatchFile &file)
{
    return file.path.rfind(StoreInstallDir(), 0) == 0;
}

static const char *StoreStateText(int state)
{
    switch (state) {
    case STORE_CHECKING: return "Checking...";
    case STORE_WORKS: return "Get";
    case STORE_OTHER: return "Other version";
    case STORE_UNSUPPORTED: return "Not supported";
    case STORE_128MB: return "Needs 128 MB";
    case STORE_NOT_NEEDED: return "Not needed";
    case STORE_NOT_IN_GAME: return "Not for your copy";
    default: return "No network";
    }
}

// The green dot: games with a patch on that works for them, worked out a
// game a frame (the checks read the disc).
void DashboardScene::RefreshPatchBadges()
{
    m_badge_next = 0;
}

void DashboardScene::PatchBadgeStep()
{
    if (m_badge_next < 0 || m_badge_next >= (int)m_games.size()) {
        m_badge_next = -1;
        return;
    }
    const Game &game = m_games[m_badge_next++];
    bool active = false;
    if (game.title_id) {
        std::vector<const PatchFile *> files = PatchesFor(game.title_id);
        if (!files.empty()) {
            PatchChoices choices = PatchChoicesLoad(KeyOf(game));
            const XisoInfo &xiso = Art(game.path).info;
            for (const PatchFile *f : files) {
                for (const auto &g : f->groups) {
                    if (!active && !g.info && PatchGroupOn(choices, *f, g) &&
                        PatchGroupProblem(*f, g, game.path, xiso.xbe_offset,
                                          xiso.xbe_size).empty()) {
                        active = true;
                    }
                }
            }
        }
    }
    m_patch_active[game.path] = active;
}

//
// Settings > Patches: every patch in use that works for one of the games,
// to turn on or off, the game's name with each.
//

void DashboardScene::OpenAllPatches()
{
    m_all_patches.clear();
    for (const auto &game : m_games) {
        if (!game.title_id) {
            continue;
        }
        const XisoInfo &xiso = Art(game.path).info;
        for (const PatchFile *f : PatchesFor(game.title_id)) {
            for (int g = 0; g < (int)f->groups.size(); g++) {
                const PatchGroup &group = f->groups[g];
                if (!group.info &&
                    PatchGroupProblem(*f, group, game.path, xiso.xbe_offset,
                                      xiso.xbe_size).empty()) {
                    m_all_patches.push_back({ game.path, game.name, KeyOf(game),
                                              f, g });
                }
            }
        }
    }
    m_all_row = 0;
    m_all_page = true;
}

void DashboardScene::AllPatchesInput(bool accept, bool left, bool right, bool up,
                                     bool down)
{
    int n = m_all_patches.size();
    if (!n) {
        return;
    }
    if (up) m_all_row = (m_all_row + n - 1) % n;
    if (down) m_all_row = (m_all_row + 1) % n;
    if (accept || left || right) {
        const auto &row = m_all_patches[m_all_row];
        PatchChoices choices = PatchChoicesLoad(row.key);
        const PatchGroup &g = row.file->groups[row.group];
        bool on = !PatchGroupOn(choices, *row.file, g);
        choices[PatchGroupId(*row.file, g)] = on;
        PatchChoicesSave(row.key, choices);
        UiSoundPlay(UI_SOUND_CHANGE);
        RefreshPatchBadges();
    }
}

void DashboardScene::DrawAllPatches(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);
    float x = 280 * s + slide, w = 1400 * s;
    if (m_all_patches.empty()) {
        Text(dl, font, 48 * s, ImVec2(x, 260 * s), White(a),
             "No patches in use for your games");
        Text(dl, small, 32 * s, ImVec2(x, 340 * s), Label(a),
             "Get them per game: Games > Triangle > Game patches.", w);
        return;
    }
    const int visible = 9;
    float h = 60 * s, gap = 72 * s, y0 = 230 * s;
    int n = m_all_patches.size();
    int top = std::clamp(m_all_row - visible / 2, 0, std::max(0, n - visible));
    std::map<std::string, PatchChoices> choices;
    for (int i = top; i < std::min(n, top + visible); i++) {
        const auto &row = m_all_patches[i];
        if (!choices.count(row.key)) {
            choices[row.key] = PatchChoicesLoad(row.key);
        }
        const PatchGroup &g = row.file->groups[row.group];
        bool on = PatchGroupOn(choices[row.key], *row.file, g);
        bool selected = i == m_all_row;
        float y = y0 + (i - top) * gap;
        Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, a);
        std::string game = row.game_name;
        if (game.size() > 26) {
            game = game.substr(0, 24) + "...";
        }
        Text(dl, small, 30 * s, ImVec2(x + 40 * s, y + 14 * s),
             selected ? Ink(a * 0.8f) : White(a * 0.6f), game.c_str());
        std::string name = g.name;
        if (name.size() > 46) {
            name = name.substr(0, 44) + "...";
        }
        Text(dl, font, 38 * s, ImVec2(x + 470 * s, y + 10 * s),
             selected ? Ink(a) : Label(a), name.c_str());
        const char *value = selected ? (on ? "<  On  >" : "<  Off  >") :
                                       (on ? "On" : "Off");
        ImVec2 vs = TextSize(font, 38 * s, value);
        Text(dl, font, 38 * s, ImVec2(x + w - vs.x - 50 * s, y + 10 * s),
             selected ? Ink(a) : on ? Lime(a) : White(a * 0.55f), value);
    }
    Focus(dl, ImVec2(x, y0 + (m_all_row - top) * gap),
          ImVec2(x + w, y0 + (m_all_row - top) * gap + h), s, m_time, a);
    const auto &sel = m_all_patches[m_all_row];
    std::string about = sel.file->file_name +
                        (sel.file->author.empty() ? "" : "  -  by " + sel.file->author);
    TextFit(dl, small, 28 * s, ImVec2(x + 10 * s, y0 + visible * gap + 8 * s),
            Label(a * 0.9f), about, w);
}

// A game's settings, top to bottom. Resolution, smoothing and the audio
// processor are the main settings' only.
static const int kGameRows[] = {
    DashboardScene::GS_NAME,    GO_ASPECT, GO_FIT, GO_VOLUME, GO_OVERLAY,
    DashboardScene::GS_PATCHES, DashboardScene::GS_SHORTCUT,
    DashboardScene::GS_RESET,
};
static const int kGameRowCount = sizeof(kGameRows) / sizeof(kGameRows[0]);

static int GameRowIndex(int row)
{
    for (int k = 0; k < kGameRowCount; k++) {
        if (kGameRows[k] == row) {
            return k;
        }
    }
    return 0;
}

void DashboardScene::OpenGameSettings(const Game &game, bool from_shelf)
{
    m_gs_open = true;
    m_gs_from_shelf = from_shelf;
    m_gs_game = game;
    m_gs_key = KeyOf(game);
    m_gs_shortcut = ShortcutDirFor(game.path);
    m_gs_profile = GameProfileLoad(m_gs_key);
    m_gs_row = kGameRows[0];
    m_gs_patches = false;
    m_patch_row = 0;
    m_gs_choices = PatchChoicesLoad(m_gs_key);
    BuildPatchItems();
    m_in_page = true;
}

// The game in Game settings' patches: each option of each file for it,
// the ones that work first.
void DashboardScene::BuildPatchItems()
{
    m_gs_patch_items.clear();
    const XisoInfo &xiso = Art(m_gs_game.path).info;
    for (const PatchFile *f : PatchesFor(m_gs_game.title_id)) {
        for (int g = 0; g < (int)f->groups.size(); g++) {
            m_gs_patch_items.push_back(
                { f, g,
                  PatchGroupProblem(*f, f->groups[g], m_gs_game.path,
                                    xiso.xbe_offset, xiso.xbe_size),
                  PatchGroupFoundInCopy(*f, f->groups[g], m_gs_game.path,
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
    m_gs_crc_ok = PatchXbeCrc(m_gs_game.path, xiso.xbe_offset, xiso.xbe_size,
                              &m_gs_crc);
}

// Rows: the options, then "Reset to defaults".
void DashboardScene::ChangeGameSetting(int step)
{
    if (m_gs_row == GS_PATCHES) {
        return; // Opens the list (HandleInputInner)
    }
    bool running = GameIsRunning(m_gs_key);
    if (m_gs_row == GO_SCALE) {
        UiSoundPlay(UI_SOUND_ERROR); // Every game uses the main setting
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
    ADV_MEMORY,
    ADV_AVPACK,
    ADV_DSP,
    ADV_FPU,
    ADV_SHADER_CACHE,
    ADV_NOTIFICATIONS,
    ADV_PINNING,
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
    return row == ADV_MEMORY || row == ADV_PINNING || row == ADV_AVPACK ||
           row == ADV_FPU || row == ADV_SHADER_CACHE;
}

// The Xbox's video modes (its EEPROM), set for each game as it starts
// (the Xbox reads them as it boots): 720p only with a 720p patch on (the
// patch needs it; other games would switch to modes they rarely get
// right), widescreen with screen shape 16:9, 480p as it is, 1080i never.
static void SetXboxVideoFor(bool patch_720p, bool widescreen)
{
    uint32_t flags;
    if (!xemu_eeprom_get_video_flags(&flags)) {
        return;
    }
    uint32_t want = flags & ~(XBOX_VIDEO_720P | XBOX_VIDEO_1080I |
                              XBOX_VIDEO_WIDESCREEN | XBOX_VIDEO_LETTERBOX);
    if (patch_720p) {
        want |= XBOX_VIDEO_720P | XBOX_VIDEO_480P;
    }
    if (widescreen) {
        want |= XBOX_VIDEO_WIDESCREEN;
    }
    if (want != flags) {
        xemu_eeprom_set_video_flags(want);
    }
    fprintf(stderr, "XPSemu: Xbox video modes %08X%s%s\n", want,
            patch_720p ? " (720p patch on)" : "",
            widescreen ? " (widescreen)" : "");
}

void DashboardScene::ChangeAdvanced(int step)
{
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
    int where[13];
    bool operator==(const FocusState &o) const
    {
        return depth == o.depth && !memcmp(where, o.where, sizeof(where));
    }
};

void DashboardScene::HandleInput()
{
    auto state = [this]() {
        return FocusState{ m_in_page + m_advanced + m_gs_open + m_gs_patches +
                               (m_settings_cat >= 0) + m_view + m_pad_page +
                               m_kb_open + m_all_page,
                           { m_page, m_game, m_setting, m_adv_setting,
                             m_gs_row + 100 * m_patch_row, m_system_row,
                             m_shelf, m_shelf_sel,
                             m_visible, m_np_focus, m_cat_sel, m_pad_sel,
                             m_kb_row * 100 + m_kb_col + m_all_row * 10000 } };
    };
    FocusState before = state();
    g_ui_sound_played = false;
    HandleInputInner();
    FocusState after = state();
    if (!(after == before)) {
        OrbPoke(1); // The orb feels every move
    }
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
    bool square = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false);

    if (m_setup) { // First run: the files, nothing else
        if (accept && m_setup_ok) {
            UiSoundPlay(UI_SOUND_LAUNCH);
            QuitApp(true); // Started again, the Xbox gets them
        } else if (accept) {
            ScanGames();
            m_setup_ok = !SetupRecheck((int)m_games.size());
            UiSoundPlay(m_setup_ok ? UI_SOUND_LAUNCH : UI_SOUND_ERROR);
        }
        return;
    }

    // The touchpad (Guide) closes the dashboard, back to the game.
    if (pressed & CONTROLLER_BUTTON_GUIDE) {
        Close();
        return;
    }

    // Quick resume: Circle held anywhere goes straight back to the game.
    if (m_can_return && m_game_started && (buttons & CONTROLLER_BUTTON_B)) {
        m_back_hold += ImGui::GetIO().DeltaTime;
        if (m_back_hold >= 0.6f) {
            m_back_hold = 0;
            Close();
            return;
        }
    } else {
        m_back_hold = 0;
    }

    // Square ends the running game (asks once more first): the disc comes
    // out and the Xbox goes back to its dashboard, as System > Eject does.
    if (!m_in_page && square && NowPlayingGame()) {
        if (m_time < m_eject_ask) {
            m_eject_ask = -1;
            StopGame("ejected from the dashboard");
        } else {
            m_eject_ask = m_time + 2.5f;
            UiSoundPlay(UI_SOUND_CHANGE);
        }
        return;
    }
    if (m_eject_ask >= 0 && (m_in_page || accept || back || up || down ||
                             left || right)) {
        m_eject_ask = -1; // Anything else: not ejecting
    }

    if (!m_in_page && m_np_focus) { // Now Playing, above the menu
        if (!NowPlayingGame() || down) {
            m_np_focus = false;
        } else if (accept || back) {
            Close();
        }
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
        if (up && m_page == 0 && NowPlayingGame()) {
            m_np_focus = true; // Up from Games: the game that's running
            return;
        }
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
                m_settings_cat = -1;
            }
        }
        if (back) Close();
        return;
    }

    if (m_gs_open && m_gs_patches) { // A game's patches
        int in_use = m_gs_patch_items.size();
        int n = in_use + (int)m_store.size();
        if (back) {
            m_gs_patches = false;
            return;
        }
        if (!n) {
            return;
        }
        if (up) m_patch_row = (m_patch_row + n - 1) % n;
        if (down) m_patch_row = (m_patch_row + 1) % n;
        if (m_patch_row >= in_use) { // The store's
            if (accept) {
                StoreGet(m_patch_row - in_use);
            }
            return;
        }
        if (square && FromStore(*m_gs_patch_items[m_patch_row].file)) {
            StoreRemove(*m_gs_patch_items[m_patch_row].file);
            return;
        }
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

    if (m_gs_open && m_kb_open) { // Its name being typed
        KeyboardInput(accept, back, square, options, up, down, left, right);
        return;
    }
    if (m_gs_open) { // A game's settings
        if (back) {
            m_gs_open = false;
            if (m_gs_from_shelf) {
                m_in_page = false;
            }
            return;
        }
        int k = GameRowIndex(m_gs_row);
        if (up) k = (k + kGameRowCount - 1) % kGameRowCount;
        if (down) k = (k + 1) % kGameRowCount;
        m_gs_row = kGameRows[k];
        if (m_gs_row == GS_NAME) {
            if (accept || right) {
                OpenNameEditor();
            }
            return;
        }
        if (m_gs_row == GS_PATCHES && (accept || right)) {
            m_gs_patches = true;
            StoreOpen();
            m_patch_row = 0;
            return;
        }
        if (m_gs_row == GS_SHORTCUT) {
            if (accept) {
                ToggleShortcut(m_gs_game);
            }
            return;
        }
        if (m_gs_row == GS_RESET ? accept : (left || right || accept)) {
            ChangeGameSetting(left ? -1 : 1);
        }
        return;
    }

    if (m_view && m_page == PAGE_GAMES) { // The Box Art Viewer has it all
        ViewerInput(accept, back, options, left, right);
        return;
    }
    if (back) {
        if (m_page == PAGE_SETTINGS && m_advanced) {
            m_advanced = false;
        } else if (m_page == PAGE_SETTINGS && m_pad_page) {
            m_pad_page = false;
        } else if (m_page == PAGE_SETTINGS && m_all_page) {
            m_all_page = false;
        } else if (m_page == PAGE_SETTINGS && m_settings_cat >= 0) {
            m_settings_cat = -1; // Back to the categories
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
        if (square) {
            OpenViewer();
            break;
        }
        if (accept) {
            CurtainFrom(m_sel0, m_sel1); // The cover flies from there
            Launch(m_games[m_game]);
        }
        else if (options) OpenGameSettings(m_games[m_game], false);
        break;
    }
    case PAGE_SETTINGS: {
        if (m_pad_page) {
            PadInput(accept, left, right, up, down, options);
            break;
        }
        if (m_all_page) {
            AllPatchesInput(accept, left, right, up, down);
            break;
        }
        if (m_advanced) {
            const int rows = ADV__COUNT;
            if (up) m_adv_setting = (m_adv_setting + rows - 1) % rows;
            if (down) m_adv_setting = (m_adv_setting + 1) % rows;
            if (left || right || accept) {
                ChangeAdvanced(left ? -1 : 1);
            }
            break;
        }
        if (m_settings_cat < 0) { // The categories
            if (up) m_cat_sel = (m_cat_sel + CAT__COUNT - 1) % CAT__COUNT;
            if (down) m_cat_sel = (m_cat_sel + 1) % CAT__COUNT;
            if (accept || right) {
                if (m_cat_sel == CAT_ADVANCED) {
                    m_advanced = true;
                } else if (m_cat_sel == CAT_CONTROLLER) {
                    m_pad_page = true;
                    m_pad_sel = 0;
                } else if (m_cat_sel == CAT_PATCHES) {
                    OpenAllPatches();
                } else {
                    m_settings_cat = m_cat_sel;
                    m_setting = kCategories[m_cat_sel].rows[0];
                }
            }
            break;
        }
        const auto &cat = kCategories[m_settings_cat];
        int at = 0;
        for (int i = 0; i < cat.count; i++) {
            if (cat.rows[i] == m_setting) {
                at = i;
            }
        }
        if (up) m_setting = cat.rows[(at + cat.count - 1) % cat.count];
        if (down) m_setting = cat.rows[(at + 1) % cat.count];
        if (left || right || accept) {
            ChangeSetting(left ? -1 : 1);
        }
        break;
    }
    case PAGE_SYSTEM:
        // Two buttons: Restart XPSemu, Eject the disc.
        m_system_row = std::clamp(m_system_row, 0, 1);
        if (up || left || down || right) {
            m_system_row ^= 1;
            m_sys_ask = -1;
        }
        if (accept && m_system_row == 0) { // Restart XPSemu
            // While a game runs, a second press: it ends the game.
            if (m_game_started && m_time >= m_sys_ask) {
                m_sys_ask = m_time + 2.5f;
                UiSoundPlay(UI_SOUND_CHANGE);
            } else {
                QuitApp(true);
            }
        }
        if (accept && m_system_row == 1) { // Eject the disc
            if (m_game_started) {
                StopGame("disc ejected");
            } else {
                ActionEjectDisc();
            }
        }
        break;
    }
}

bool DashboardScene::Draw()
{
    ImGuiIO &io = ImGui::GetIO();
    float dt = io.DeltaTime;
    m_time += dt;
    if (m_boot) {
        // Steps of at most a 30 fps frame (startup stalls), X or O skips.
        float was = m_boot_t;
        m_boot_t += std::min(dt, 1 / 30.0f);
        if (was < 2.0f && m_boot_t >= 2.0f) { // The emblem's flash
            UiSoundPlay(UI_SOUND_FLASH);
        }
        bool skip = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                    ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        if (skip && m_boot_t < kBootLen - 0.5f) {
            m_boot_t = kBootLen - 0.5f;
        }
        if (m_boot_t >= kBootLen) {
            m_boot = false;
            m_music_at = m_time; // The dashboard fades in, the music with it
        } else {
            DrawBoot(io.DisplaySize.y / 1080.0f);
        }
    }
    {
        bool idle;
        int failed;
        for (const auto &got : CoverDownloadTake(&idle, &failed)) {
            if (got.part != COVER_FRONT) { // For the viewer: load it now
                for (const auto &game : m_games) {
                    if (game.title_id == got.title_id) {
                        GameArt &art = Art(game.path);
                        (got.part == COVER_BACK ? art.back_tried :
                                                  art.spine_tried) = false;
                    }
                }
                continue;
            }
            char id[16];
            snprintf(id, sizeof(id), "%08x", got.title_id);
            g_covers[id] = got.path;
            for (const auto &game : m_games) {
                if (game.title_id == got.title_id && !Art(game.path).cover) {
                    Art(game.path).cover_tried = false;
                }
            }
            m_covers_new++;
        }
        if (idle && m_covers_new) {
            xemu_queue_notification(
                m_covers_new == 1 ? "1 new cover downloaded" :
                (std::to_string(m_covers_new) + " new covers downloaded")
                    .c_str());
            m_covers_new = 0;
        } else if (idle && failed && !m_covers_told_offline) {
            m_covers_told_offline = true; // Once a run
            xemu_queue_notification(
                "Couldn't download covers (no internet?). Trying again later");
        }
    }
    // The games folders, every 2 s: drives plugged in or pulled out, games
    // copied in. Not while a game's page, the viewer or the keyboard is
    // open (they hold the game); the selection stays on the same game.
    if (m_visible && !m_boot && !m_setup && m_time >= m_games_next) {
        m_games_next = m_time + 2;
        if (!m_gs_open && !m_view && !m_kb_open && !m_all_page &&
            GamesSignature() != m_games_sig) {
            std::string selected = m_games.empty() ? "" :
                m_games[std::clamp(m_game, 0, (int)m_games.size() - 1)].path;
            size_t before = m_games.size();
            ScanGames();
            for (size_t i = 0; i < m_games.size(); i++) {
                if (m_games[i].path == selected) {
                    m_game = (int)i;
                    m_game_anim = m_game;
                }
            }
            if (m_games.size() != before) {
                int diff = (int)m_games.size() - (int)before;
                xemu_queue_notification(
                    (diff > 0 ? std::to_string(diff) + (diff == 1 ? " game" : " games") +
                                    " added" :
                                std::to_string(-diff) + (diff == -1 ? " game" : " games") +
                                    " removed")
                        .c_str());
            }
        }
    }
    if (!m_autolaunch.empty() && !m_boot && !m_setup && m_visible) {
        std::string path = m_autolaunch;
        m_autolaunch.clear();
        bool found = false;
        for (const auto &game : m_games) {
            if (game.path == path) {
                Launch(game, true); // Straight in, no dashboard on the way
                found = true;
                break;
            }
        }
        if (!found) {
            xemu_queue_notification(
                ("This shortcut's game isn't there any more: " + path).c_str());
        }
    }
    {
        std::lock_guard<std::mutex> guard(g_shortcut_lock);
        if (!g_shortcut_message.empty()) {
            xemu_queue_notification(g_shortcut_message.c_str());
            g_shortcut_message.clear();
            if (m_gs_open) {
                m_gs_shortcut = ShortcutDirFor(m_gs_game.path);
            }
        }
    }
    if (m_music_at >= 0 && m_time >= m_music_at) {
        m_music_at = -1;
        UiMusicPlay(m_visible);
    }
    if (m_startup_fade && m_visible) {
        // At startup the screen goes black, then the dashboard eases in
        // from it, as Ruffle Flash's library does. The first frames after
        // Show stall (games scanned, covers loaded): the fade waits for a few
        // smooth frames and steps at most a 30 fps frame at a time, so a
        // stall can't make it jump to the end.
        const float fade_in = 1.8f, to_black = 0.4f;
        float step = std::min(dt, 1 / 30.0f);
        m_startup_black = std::min(m_startup_black + step / to_black, 1.0f);
        m_startup_smooth = dt < 1 / 20.0f ? m_startup_smooth + 1 : 0;
        if (m_startup_black >= 1 && m_startup_smooth >= 8 && !m_boot) {
            m_startup_settled = true;
        }
        if (m_startup_settled) {
            m_startup_fade_elapsed =
                std::min(m_startup_fade_elapsed + step, fade_in);
        }
        float t = m_startup_fade_elapsed / fade_in;
        m_alpha = t * t * (3.0f - 2.0f * t);
        if (m_startup_fade_elapsed >= fade_in) {
            m_startup_fade = false;
        }
    } else {
        m_startup_fade = false;
        m_alpha = Approach(m_alpha, m_visible ? 1 : 0, dt, 8);
    }
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
    UploadDecoded();
    if (m_gs_open && m_gs_patches) {
        StorePoll();
    }
    if (false && m_badge_next >= 0) { // No badges on the covers now
        PatchBadgeStep();
    }
    bool attract = UpdateAttract(dt);
    if (m_pad_capture) {
        PadCapture(); // Waiting for a DualSense button: nothing else
    } else if (m_visible && !attract && !m_boot) {
        HandleInput();
    }
    // The orb: towards the focus (and the left stick), the extra spin
    // dying away, asleep after 20 s without a touch, awake at once.
    {
        auto analog = [](ImGuiKey k) { return ImGui::GetKeyData(k)->AnalogValue; };
        float sx = analog(ImGuiKey_GamepadLStickRight) -
                   analog(ImGuiKey_GamepadLStickLeft);
        float sy = analog(ImGuiKey_GamepadLStickDown) -
                   analog(ImGuiKey_GamepadLStickUp);
        float focus = m_np_focus ? -1.3f : m_shelf ? 1.4f :
                                   (m_page - (MENU__COUNT - 1) / 2.0f) / 1.5f;
        m_orb_lean_x = Approach(m_orb_lean_x, 0.4f + sx, dt, 5);
        m_orb_lean_y = Approach(m_orb_lean_y, focus * 0.7f + sy, dt, 5);
        float sleepy = std::clamp((m_idle - 20) / 6, 0.f, 1.f);
        if (m_orb_sleep > 0.3f && sleepy == 0) {
            m_orb_wake = m_time; // Woken: a flash and a big ripple
            OrbPoke(2.5f);
        }
        m_orb_sleep = Approach(m_orb_sleep, sleepy, dt,
                              sleepy > m_orb_sleep ? 0.6f : 8);
        m_orb_spin *= expf(-2.5f * dt);
        const float base[2] = { 0.35f, -0.5f };
        for (int i = 0; i < 2; i++) {
            m_orb_phase[i] += dt * (base[i] * (1 - 0.8f * m_orb_sleep) +
                                    (i ? -m_orb_spin : m_orb_spin));
        }
    }
    m_page_anim = Approach(m_page_anim, m_in_page ? 1 : 0, dt, 10);
    m_menu_anim = Approach(m_menu_anim, m_page, dt, 16);
    // The Games carousel glides on a spring, as the Xbox 360's did: quick
    // off the mark, settling without a bounce (critically damped). Small
    // steps keep it steady through a slow frame.
    {
        float left = std::min(dt, 0.1f);
        while (left > 0) {
            float h = std::min(left, 1 / 240.f);
            float pull = 320 * (m_game - m_game_anim) - 36 * m_game_vel;
            m_game_vel += pull * h;
            m_game_anim += m_game_vel * h;
            left -= h;
        }
        if (fabsf(m_game - m_game_anim) < 0.001f && fabsf(m_game_vel) < 0.01f) {
            m_game_anim = m_game;
            m_game_vel = 0;
        }
    }
    m_shelf_anim = Approach(m_shelf_anim, m_shelf_sel, dt, 16);
    m_shelf_focus = Approach(m_shelf_focus, m_shelf && !m_in_page ? 1 : 0, dt,
                             10);


    DrawBackground(s);
    if (m_setup) {
        DrawSetup(s, m_alpha);
        DrawHints(s);
        DrawStatus(s);
        ImGui::End();
        return true;
    }
    // The main menu slides out to the left as a page slides in.
    if (m_page_anim < 0.99f) {
        DrawMainMenu(s, m_alpha * (1 - m_page_anim));
    }
    if (m_page_anim > 0.01f) {
        float a = m_alpha * m_page_anim;
        DrawPageHeader(s, a);
        if (m_gs_open) {
            DrawGameSettings(s, a);
            if (m_kb_open) {
                DrawKeyboard(s, a);
            }
        } else switch (m_page) {
        case PAGE_GAMES: DrawGames(s, a); break;
        case PAGE_SETTINGS:
            if (m_pad_page) {
                DrawPadPage(s, a);
            } else if (m_all_page) {
                DrawAllPatches(s, a);
            } else if (m_advanced) {
                DrawAdvanced(s, a);
            } else {
                DrawSettings(s, a);
            }
            break;
        case PAGE_SYSTEM: DrawSystem(s, a); break;
        }
    }
    DrawHints(s);
    DrawStatus(s);
    if (m_attract_alpha > 0.01f) {
        DrawAttract(s, m_alpha * m_attract_alpha);
    }

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

    // At startup: black over the Xbox's screen first, the dashboard over it.
    if (m_startup_fade) {
        dl->AddRectFilled(ImVec2(0, 0), size, Rgba(0, 0, 0, m_startup_black));
    }


    float ground = a;

    // Dark green, a little lighter in the middle.
    dl->AddRectFilledMultiColor(ImVec2(0, 0), size, Rgba(4, 40, 4, ground),
                                Rgba(2, 26, 2, ground), Rgba(0, 12, 0, ground),
                                Rgba(2, 24, 2, ground));

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
// A move (or waking up): the orb's rings spin faster for a moment and a
// ripple runs out from it.
void DashboardScene::OrbPoke(float strength)
{
    m_orb_spin = std::min(m_orb_spin + 1.6f * strength, 6.f);
    int oldest = 0;
    for (int i = 1; i < 4; i++) {
        if (m_orb_ripple[i] < m_orb_ripple[oldest]) {
            oldest = i;
        }
    }
    m_orb_ripple[oldest] = m_time;
}

void DashboardScene::DrawOrb(float cx, float cy, float r)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float a = m_alpha;
    // Leaning a little towards the focus and the stick; the light on it
    // shifting the other way.
    ImVec2 c(cx + m_orb_lean_x * r * 0.05f, cy + m_orb_lean_y * r * 0.06f);
    // Asleep: dimmer, breathing slow and deep; waking: a flash of light.
    float sleep = m_orb_sleep;
    float wake = expf(-std::max(m_time - m_orb_wake, 0.f) * 3);
    float bright = 1 - 0.55f * sleep + 0.35f * wake;
    auto lit = [&](int r_, int g_, int b_, float al) {
        return Rgba((int)(r_ * bright), (int)(g_ * bright), (int)(b_ * bright),
                    al);
    };
    float breathe = 1 + (0.02f + 0.035f * sleep) *
                            sinf(m_time * (2 - 1.6f * sleep));

    // Halo, then the sphere from its rim to its bright core.
    dl->AddCircleFilled(c, r * 1.35f * breathe,
                        lit(90, 200, 30, a * (0.10f + 0.08f * wake)), 64);
    dl->AddCircleFilled(c, r * 1.15f, lit(90, 200, 30, a * 0.14f), 64);
    dl->AddCircleFilled(c, r * breathe, lit(40, 140, 15, a), 64);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.08f, c.y - r * 0.08f), r * 0.82f,
                        lit(110, 200, 25, a), 64);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.15f, c.y - r * 0.15f), r * 0.6f,
                        lit(175, 230, 35, a), 64);
    dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y - r * 0.2f), r * 0.38f,
                        lit(225, 245, 70, a), 48);
    // Shine.
    dl->AddCircleFilled(ImVec2(c.x - r * (0.42f + 0.06f * m_orb_lean_x),
                               c.y - r * (0.45f + 0.06f * m_orb_lean_y)),
                        r * 0.1f, White(a * 0.8f * (1 - 0.5f * sleep)), 24);

    // Ripples running out from it, one per move.
    for (float started : m_orb_ripple) {
        float q = (m_time - started) / 0.8f;
        if (q < 0 || q > 1) {
            continue;
        }
        float e = 1 - (1 - q) * (1 - q);
        dl->AddCircle(c, r * (1.05f + 0.85f * e), Lime(a * (1 - q) * 0.55f), 72,
                      r * 0.035f * (1 - q) + 1);
    }

    // Two tilted rings turning around it, like the original's ribbons.
    for (int ring = 0; ring < 2; ring++) {
        float spin = m_orb_phase[ring];
        float tilt = ring ? 0.35f : 0.22f;
        const int n = 64;
        for (int i = 0; i < n; i++) {
            float t0 = spin + 2 * M_PI * i / n, t1 = spin + 2 * M_PI * (i + 1) / n;
            float z = sinf((t0 + t1) / 2); // Front of the ring is brighter
            ImVec2 p0(c.x + cosf(t0) * r * 1.22f,
                      c.y + sinf(t0) * r * 1.22f * tilt + cosf(t0) * r * 0.25f);
            ImVec2 p1(c.x + cosf(t1) * r * 1.22f,
                      c.y + sinf(t1) * r * 1.22f * tilt + cosf(t1) * r * 0.25f);
            dl->AddLine(p0, p1,
                        Lime(a * (1 - 0.5f * sleep) *
                             (0.25f + 0.6f * (z + 1) / 2)),
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
    DrawNowPlaying(s, ma, x, w, y0 - 26 * s);
    for (int i = 0; i < MENU__COUNT; i++) {
        float y = y0 + i * gap;
        bool selected = i == m_page && !m_shelf && !m_np_focus;
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
    if (k < 0.5f && !m_np_focus) {
        float fy = y0 + m_menu_anim * gap;
        Focus(dl, ImVec2(x, fy), ImVec2(x + w, fy + h), s, m_time,
              a * (1 - 2 * k));
    }

    if (shelf) {
        DrawShelf(s, a, 150 * s + slide, 680 * s);
    }
    m_alpha = save_alpha;
}

const DashboardScene::Game *DashboardScene::NowPlayingGame()
{
    if (!m_game_started || !g_watch.active || g_watch.exit_told ||
        m_now_playing_path.empty()) {
        return nullptr;
    }
    for (const auto &g : m_games) {
        if (g.path == m_now_playing_path) {
            return &g;
        }
    }
    return nullptr;
}

// The game running behind the dashboard: a slim bar above the menu, in the
// menu's shape and column, with a live light in a socket like the menu's.
void DashboardScene::DrawNowPlaying(float s, float a, float x, float w,
                                    float bottom)
{
    const Game *it = NowPlayingGame();
    if (!it) {
        return;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float h = 92 * s;
    ImVec2 p0(x, bottom - h), p1(x + w, bottom);
    float cy = p0.y + h / 2;
    float cut = h * 0.3f;

    // The socket, its light pulsing: the game is live.
    float pulse = 0.5f + 0.5f * sinf(m_time * 3);
    ImVec2 sc(x - 70 * s, cy);
    dl->AddCircle(sc, 30 * s, Line(a), 40, 3 * s);
    dl->AddCircleFilled(sc, 22 * s, Rgba(8, 36, 6, a), 40);
    dl->AddCircle(sc, (14 + 12 * pulse) * s, Lime(a * 0.4f * (1 - pulse)), 32,
                  2 * s);
    dl->AddCircleFilled(sc, 10 * s, Lime(a * (0.6f + 0.4f * pulse)), 24);
    dl->AddLine(ImVec2(sc.x + 30 * s, cy), ImVec2(x, cy), Line(a), 2 * s);

    // The bar: the menu's angled shape, a shade darker.
    BarPath(dl, p0, p1, cut);
    dl->PathFillConvex(Rgba(4, 28, 4, a * 0.85f));
    BarPath(dl, p0, p1, cut);
    dl->PathStroke(Line(a * 0.9f), ImDrawFlags_Closed, 2 * s);
    if (m_np_focus && !m_shelf) {
        Focus(dl, p0, p1, s, m_time, a);
    }

    // The cover, small, standing in the bar.
    float ch = h - 22 * s, cw = ch * 0.72f;
    ImVec2 c0(x + cut + 10 * s, p0.y + 11 * s), c1(c0.x + cw, c0.y + ch);
    XemuTexture cover = GameCover(*it);
    if (cover) {
        dl->AddImageRounded((ImTextureID)cover, c0, c1, ImVec2(0, 0),
                            ImVec2(1, 1), White(a), 4 * s);
        dl->AddRect(c0, c1, Line(a), 4 * s, 0, 1.5f * s);
    } else {
        DrawGameArt(*it, c0, c1, a, false);
    }

    // On the right: back to the game, then the time played before it.
    const char *back = "Back to game";
    float bsz = 24 * s;
    ImVec2 bs = TextSize(small, bsz, back);
    float bx = p1.x - cut - 14 * s - bs.x;
    Text(dl, small, bsz, ImVec2(bx, cy - bs.y / 2), White(a * 0.85f), back);
    DrawGlyph(dl, ImVec2(bx - 24 * s, cy), 14 * s, GLYPH_CIRCLE, a * 0.9f);
    bool asking = m_time < m_eject_ask;
    const char *eject = asking ? "Again to eject" : "Eject";
    ImVec2 es = TextSize(small, bsz, eject);
    float ex = bx - 56 * s - es.x;
    float blink = asking ? 0.6f + 0.4f * sinf(m_time * 9) : 0.85f;
    Text(dl, small, bsz, ImVec2(ex, cy - es.y / 2),
         asking ? Lime(a * blink) : White(a * 0.85f), eject);
    DrawGlyph(dl, ImVec2(ex - 24 * s, cy), 14 * s, GLYPH_SQUARE, a * 0.9f);
    float sep = ex - 56 * s;
    dl->AddLine(ImVec2(sep, p0.y + 24 * s), ImVec2(sep, p1.y - 24 * s),
                Line(a * 0.6f), 1.5f * s);

    std::string time_text = GamePlayTimeText(GamePlayGet(KeyOf(*it)).seconds);
    float tsz = 28 * s;
    ImVec2 ts = TextSize(small, tsz, time_text.c_str());
    float tx = sep - 26 * s - ts.x;
    Text(dl, small, tsz, ImVec2(tx, cy - ts.y / 2), Lime(a),
         time_text.c_str());
    ImVec2 cc(tx - 22 * s, cy);
    dl->AddCircle(cc, 11 * s, Lime(a), 20, 2.2f * s);
    dl->AddLine(cc, ImVec2(cc.x, cc.y - 6 * s), Lime(a), 2.2f * s);
    dl->AddLine(cc, ImVec2(cc.x + 5 * s, cc.y), Lime(a), 2.2f * s);

    // On the left: NOW PLAYING over the name, cut short before the time.
    float lx = c1.x + 22 * s, room = cc.x - 34 * s - lx;
    Text(dl, small, 20 * s, ImVec2(lx, p0.y + 13 * s), Lime(a * 0.9f),
         "NOW PLAYING");
    float nsz = 34 * s;
    std::string name = it->name;
    if (TextSize(font, nsz, name.c_str()).x > room) {
        while (!name.empty() &&
               TextSize(font, nsz, (name + "...").c_str()).x > room) {
            name.pop_back();
        }
        while (!name.empty() && name.back() == ' ') {
            name.pop_back();
        }
        name += "...";
    }
    Text(dl, font, nsz, ImVec2(lx, p0.y + 38 * s), White(a), name.c_str());
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

        // The reflection of a cover: its bottom, flipped, fading out.
        XemuTexture tex = Art(recent[i]->path).cover;
        {
            float rh = 46 * s;
            ImVec2 r0(p0.x, by + bh + 5 * s), r1(p1.x, by + bh + 5 * s + rh);
            int v0 = dl->VtxBuffer.Size;
            if (tex) {
                dl->AddImageQuad((ImTextureID)tex, r0, ImVec2(r1.x, r0.y), r1,
                                 ImVec2(r0.x, r1.y), ImVec2(0, 1), ImVec2(1, 1),
                                 ImVec2(1, 1 - rh / h), ImVec2(0, 1 - rh / h),
                                 White(ia * 0.38f));
            } else {
                dl->AddQuadFilled(r0, ImVec2(r1.x, r0.y), r1,
                                  ImVec2(r0.x, r1.y), Rgba(40, 120, 24, ia * 0.38f));
            }
            FadeDown(dl, v0, r0.y, r1.y);
            dl->AddLine(r0, ImVec2(r1.x, r0.y), White(ia * 0.25f), 1.5f * s);
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
        for (int k = 0; k < kGameRowCount; k++) {
            int i = kGameRows[k];
            float y = y0 + k * gap;
            bool selected = i == m_gs_row;
            Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, a);
            const char *name = i < GO__COUNT    ? GameOptionName(i) :
                               i == GS_NAME     ? "Name" :
                               i == GS_PATCHES  ? "Game patches" :
                               i == GS_SHORTCUT ? "Create Shortcut" :
                                                  "Reset to defaults";
            Text(dl, font, 40 * s, ImVec2(x + 40 * s, y + 9 * s),
                 selected ? Ink(a) : Label(a), name);
            char value[64] = "";
            ImU32 col = selected ? Ink(a) : Lime(a);
            if (i == GS_NAME) {
                std::string n = m_gs_game.name;
                if (n.size() > 34) {
                    n = n.substr(0, 31) + "...";
                }
                snprintf(value, sizeof(value), selected ? "%s  >" : "%s", n.c_str());
                if (!selected && !GameOwnName(KeyOf(m_gs_game))) {
                    col = White(a * 0.55f); // Its own name
                }
            } else if (i == GS_SHORTCUT) {
                bool busy;
                {
                    std::lock_guard<std::mutex> guard(g_shortcut_lock);
                    busy = g_shortcut_busy;
                }
                snprintf(value, sizeof(value), "%s",
                         busy ? "Working..." :
                         m_gs_shortcut.empty() ? (selected ? "Create  >" : "Create") :
                                                 "On the home screen");
                if (!selected && m_gs_shortcut.empty()) {
                    col = White(a * 0.55f);
                }
            } else if (i == GS_PATCHES) {
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
                bool locked = i == GO_SCALE; // The main setting's
                int v = i == GO_SCALE ? GAME_DEFAULT : m_gs_profile.value[i];
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
        float fy = y0 + GameRowIndex(m_gs_row) * gap;
        Focus(dl, ImVec2(x, fy), ImVec2(x + w, fy + h), s, m_time, a);

        const char *help =
            m_gs_row < GO__COUNT ? GameOptionHelp(m_gs_row) :
            m_gs_row == GS_PATCHES ?
                "Widescreen, 60 FPS and other fixes from .JMP files in "
                "/data/xemu/patches, applied as the game loads: the disc "
                "image isn't changed." :
            m_gs_row == GS_NAME ?
                "The name shown for it everywhere in XPSemu. Clear it for "
                "its own name." :
            m_gs_row == GS_SHORTCUT ?
                (m_gs_shortcut.empty() ?
                     "A tile of its own on the PS5's home screen (installed by "
                     "ShadowMountPlus): it opens XPSemu straight into this game." :
                     "X removes it (then delete its tile from the home screen "
                     "too).") :
                "Back to Default for every setting: this game uses your "
                "Settings again (patches stay as they are).";
        TextFit(dl, small, 30 * s, ImVec2(x + 10 * s, y0 + kGameRowCount * gap + 8 * s),
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

    if (m_gs_patch_items.empty() && m_store.empty()) {
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
    int in_use = m_gs_patch_items.size();
    int n = in_use + (int)m_store.size();
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
        float y = y0 + (i - top) * gap;
        bool selected = i == m_patch_row;
        if (i >= in_use) { // From the store
            const StoreItem &st = m_store[i - in_use];
            const PatchCatalogEntry &e = kPatchCatalog[st.index];
            bool can = st.state == STORE_WORKS;
            Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected,
                can ? a : a * 0.6f);
            std::string name = std::string("Get:  ") + e.category + "  -  " +
                               e.author;
            if (e.region[0]) {
                name += std::string(" (") + e.region + ")";
            }
            const char *value = StoreStateText(st.state);
            ImVec2 vs = TextSize(font, 40 * s, value);
            while (TextSize(font, 40 * s, name.c_str()).x > w - vs.x - 130 * s &&
                   name.size() > 8) {
                name = name.substr(0, name.size() - 4) + "...";
            }
            Text(dl, font, 40 * s, ImVec2(x + 40 * s, y + 9 * s),
                 selected ? Ink(a) : Label(can ? a : a * 0.6f), name.c_str());
            Text(dl, font, 40 * s, ImVec2(x + w - vs.x - 50 * s, y + 9 * s),
                 selected ? Ink(a) : can ? Lime(a) : White(a * 0.5f), value);
            continue;
        }
        const auto &item = m_gs_patch_items[i];
        const PatchGroup &g = item.file->groups[item.group];
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
    if (m_patch_row >= in_use) {
        const StoreItem &st = m_store[m_patch_row - in_use];
        const PatchCatalogEntry &e = kPatchCatalog[st.index];
        std::string about = std::string(e.title) + "  -  " + e.category +
                            " by " + e.author + "  (Jay's Magic Patches)";
        std::string detail =
            st.state == STORE_WORKS ? std::string("Works with your copy. ") + e.notes :
            st.state == STORE_CHECKING ? "Checking it against your copy..." :
            st.state == STORE_OTHER ?
                "Made for another release of this game (other checksums)." :
            st.state == STORE_UNSUPPORTED ? "Not supported: " + st.why + "." :
            st.state == STORE_NOT_NEEDED ?
                "Not needed: XPSemu's Resolution setting already draws every "
                "game sharper than 720p, without the risk." :
            st.state == STORE_128MB ?
                "Needs the Xbox's 128 MB memory; XPSemu gives it 64 MB." :
            st.state == STORE_NOT_IN_GAME ?
                "The code it changes isn't in your copy." :
                "Couldn't download it (no network?).";
        float ty = y0 + visible * gap + 6 * s;
        ty = TextFit(dl, small, 30 * s, ImVec2(x + 10 * s, ty), Label(a), about, w);
        TextFit(dl, small, 28 * s, ImVec2(x + 10 * s, ty + 8 * s),
                st.state == STORE_WORKS ? White(a * 0.7f) : Lime(a), detail, w);
        return;
    }
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
    ty = TextFit(dl, small, 30 * s, ImVec2(x + 10 * s, ty), Label(a), about, w);
    if (!detail.empty()) {
        TextFit(dl, small, 28 * s, ImVec2(x + 10 * s, ty + 8 * s),
                sel.problem.empty() || sel.problem == "Info" ? White(a * 0.7f) :
                                                               Lime(a),
                detail, w);
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
    } else if (m_page == PAGE_SETTINGS && m_pad_page) {
        m_header = "Controller setup";
    } else if (m_page == PAGE_SETTINGS && m_all_page) {
        m_header = "Patches";
    } else if (m_page == PAGE_SETTINGS && m_settings_cat >= 0) {
        m_header = std::string(kCategories[m_settings_cat].name) + " settings";
    } else if (m_page >= 0 && m_page < PAGE__COUNT) {
        m_header = names[m_page];
    }
    std::string title = Upper(m_header.c_str());
    Text(dl, font, 64 * s, ImVec2(p0.x + 50 * s, p0.y + 16 * s), Ink(a),
         title.c_str());
    m_alpha = save_alpha;
}

// A game's name in lines that fit width w, at most max_lines (the last one
// cut short with "..." if the name doesn't fit).
static std::vector<std::string> WrapName(ImFont *font, float size,
                                         const std::string &name, float w,
                                         int max_lines)
{
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < name.size()) {
        size_t j = name.find(' ', i);
        std::string word = name.substr(i, j == std::string::npos ? j : j - i);
        i = j == std::string::npos ? name.size() : j + 1;
        std::string next = line.empty() ? word : line + " " + word;
        if (line.empty() || TextSize(font, size, next.c_str()).x <= w) {
            line = next;
            continue;
        }
        lines.push_back(line);
        line = word;
    }
    if (!line.empty()) {
        lines.push_back(line);
    }
    bool cut = (int)lines.size() > max_lines;
    if (cut) {
        lines.resize(max_lines);
    }
    for (size_t k = 0; k < lines.size(); k++) {
        std::string &l = lines[k];
        bool last_cut = cut && k + 1 == lines.size();
        if (!last_cut && TextSize(font, size, l.c_str()).x <= w) {
            continue;
        }
        // Shorter by whole UTF-8 characters until it fits with "...".
        while (!l.empty() &&
               TextSize(font, size, (l + "...").c_str()).x > w) {
            l.pop_back();
            while (!l.empty() && ((unsigned char)l.back() & 0xC0) == 0x80) {
                l.pop_back();
            }
            if (!l.empty() && ((unsigned char)l.back() & 0xC0) == 0xC0) {
                l.pop_back(); // A lead byte left without its continuation
            }
        }
        l += "...";
    }
    return lines;
}

// A game case in any four-cornered shape (tl, tr, br, bl clockwise), so the
// cases can lean while the shelf moves: the cover picture, else a card of
// XPSemu's own (dark glass lit green from the top, the disc's title image
// or a disc, and the game's name). Returns the cover texture drawn, if any,
// for the reflection.
static XemuTexture DrawCase(const DashboardScene::Game &game, ImVec2 tl,
                            ImVec2 tr, ImVec2 br, ImVec2 bl, float a,
                            float dim, ImDrawList *dl)
{
    if (!dl) {
        dl = ImGui::GetWindowDrawList();
    }
    float s = ImGui::GetIO().DisplaySize.y / 1080.0f;
    ImU32 tint = Rgba((int)(255 * dim), (int)(255 * dim), (int)(255 * dim), a);

    XemuTexture cover = GameCover(game);
    // A cover fades in over the card when it comes (loaded or downloaded).
    float fade = cover ? std::clamp((float)(ImGui::GetTime() -
                                            Art(game.path).cover_time) / 0.5f,
                                    0.f, 1.f) :
                         0;
    if (cover && fade >= 1) {
        dl->AddImageQuad((ImTextureID)cover, tl, tr, br, bl, ImVec2(0, 0),
                         ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1), tint);
        dl->AddQuad(tl, tr, br, bl, Line(a * dim * 0.9f), 1.5f * s);
        return cover;
    }

    auto lerp = [](ImVec2 p, ImVec2 q, float t) {
        return ImVec2(p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t);
    };
    float w = tr.x - tl.x, h = bl.y - tl.y;

    // Dark glass, with green light falling from the top.
    dl->AddQuadFilled(tl, tr, br, bl,
                      Rgba((int)(8 * dim), (int)(22 * dim), (int)(8 * dim),
                           a));
    int v0 = dl->VtxBuffer.Size;
    ImVec2 ml = lerp(tl, bl, 0.7f), mr = lerp(tr, br, 0.7f);
    dl->AddQuadFilled(tl, tr, mr, ml,
                      Rgba((int)(70 * dim), (int)(170 * dim), (int)(35 * dim),
                           a * 0.45f));
    FadeDown(dl, v0, std::min(tl.y, tr.y), std::max(ml.y, mr.y));
    // A sheen across the top corner, as on glass.
    dl->AddTriangleFilled(tl, lerp(tl, tr, 0.55f), lerp(tl, bl, 0.3f),
                          White(a * dim * 0.05f));

    // The disc's title image in the upper part, else a disc.
    ImVec2 c = lerp(lerp(tl, tr, 0.5f), lerp(bl, br, 0.5f), 0.4f);
    float side = std::min(w, h) * 0.5f;
    XemuTexture image = GameImage(game);
    if (image) {
        float r = 10 * s * (w / (360 * s));
        dl->AddRectFilled(ImVec2(c.x - side / 2 - 6 * s, c.y - side / 2 - 6 * s),
                          ImVec2(c.x + side / 2 + 6 * s, c.y + side / 2 + 6 * s),
                          Rgba(0, 0, 0, a * 0.35f), r + 4 * s);
        dl->AddImageRounded((ImTextureID)image,
                            ImVec2(c.x - side / 2, c.y - side / 2),
                            ImVec2(c.x + side / 2, c.y + side / 2),
                            ImVec2(0, 0), ImVec2(1, 1), tint, r);
    } else {
        float r = side * 0.46f;
        dl->AddCircleFilled(c, r, Rgba((int)(20 * dim), (int)(70 * dim),
                                       (int)(14 * dim), a), 48);
        dl->AddCircle(c, r, Lime(a * dim * 0.8f), 48, 2 * s);
        dl->AddCircle(c, r * 0.62f, Line(a * dim * 0.5f), 40, 1.5f * s);
        dl->AddCircleFilled(c, r * 0.18f, Rgba(4, 16, 4, a), 24);
        dl->AddCircle(c, r * 0.18f, Lime(a * dim * 0.8f), 24, 2 * s);
    }

    // The name, centred in the lower part (left out when too small to read).
    if (w > 110 * s) {
        ImFont *font = g_font_mgr.m_menu_font_small;
        float size = std::max(w * 0.085f, 16 * s);
        auto lines = WrapName(font, size, game.name, w * 0.84f, 3);
        float lh = size * 1.12f;
        ImVec2 nc = lerp(lerp(tl, tr, 0.5f), lerp(bl, br, 0.5f), 0.8f);
        float y = nc.y - lh * lines.size() / 2;
        for (const auto &l : lines) {
            float lw = TextSize(font, size, l.c_str()).x;
            Text(dl, font, size, ImVec2(nc.x - lw / 2, y),
                 White(a * dim * 0.92f), l.c_str());
            y += lh;
        }
    }
    if (cover) {
        dl->AddImageQuad((ImTextureID)cover, tl, tr, br, bl, ImVec2(0, 0),
                         ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1),
                         Rgba((int)(255 * dim), (int)(255 * dim),
                              (int)(255 * dim), a * fade));
    }
    dl->AddQuad(tl, tr, br, bl, Line(a * dim * 0.9f), 1.5f * s);
    return cover;
}

//
// The Box Art Viewer: Square on a game in Games lifts its case out of the
// row and turns it into a real 3D Xbox case in the middle of the screen —
// front cover, back cover and spine (xdb's, downloaded when first looked
// at), green plastic edges — to turn over in your hands: the left stick
// turns it (let go mid-turn and it carries on spinning, slowing), the right
// stick brings it closer, X flips it over, Triangle puts it straight,
// left/right on the d-pad brings the next game's case spinning in, Circle
// puts it back. Lit from the top left, with a gloss sliding across the
// covers as it turns, a shadow under it, and the cover blurred behind.
//

// A game's back cover or spine: /data/xemu/covers/<TITLEID>.back.jpg /
// .spine.jpg, downloaded from xdb the first time it's wanted.
static XemuTexture GamePart(const DashboardScene::Game &game, CoverPart part)
{
    GameArt &art = Art(game.path);
    XemuTexture &tex = part == COVER_BACK ? art.back : art.spine;
    bool &tried = part == COVER_BACK ? art.back_tried : art.spine_tried;
    if (!tried && game.title_id) {
        tried = true;
        art.parts_asked = ImGui::GetTime();
        char file[64];
        snprintf(file, sizeof(file), "/data/xemu/covers/%08X%s", game.title_id,
                 part == COVER_BACK ? ".back.jpg" : ".spine.jpg");
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) {
            DecodeRequest({ game.path, file, 1024, part });
        } else {
            CoverDownloadRequest({ { game.title_id, {}, part } },
                                 "/data/xemu/covers");
        }
    }
    return tex;
}

// A quad in four colours (one per corner), for gradients on any shape.
static void ShadedQuad(ImDrawList *dl, const ImVec2 p[4], const ImU32 col[4])
{
    ImVec2 uv = dl->_Data->TexUvWhitePixel;
    dl->PrimReserve(6, 4);
    ImDrawIdx i0 = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteIdx(i0);
    dl->PrimWriteIdx(i0 + 1);
    dl->PrimWriteIdx(i0 + 2);
    dl->PrimWriteIdx(i0);
    dl->PrimWriteIdx(i0 + 2);
    dl->PrimWriteIdx(i0 + 3);
    for (int k = 0; k < 4; k++) {
        dl->PrimWriteVtx(p[k], uv, col[k]);
    }
}

void DashboardScene::OpenViewer()
{
    m_view = true;
    m_view_closing = false;
    m_view_t = m_time;
    m_view_yaw = m_view_pitch = m_view_vyaw = m_view_vpitch = 0;
    m_view_zoom = m_view_zoom_to = 1;
    m_view_flipping = false;
    m_view_from0 = m_sel0;
    m_view_from1 = m_sel1;
}

void DashboardScene::ViewerInput(bool accept, bool back, bool options, bool left,
                                 bool right)
{
    if (m_view_closing) {
        return;
    }
    const float pi = (float)M_PI;
    if (back) {
        m_view_closing = true;
        m_view_t = m_time;
        // From where it is as drawn, to straight (the short way round).
        m_view_close_yaw = m_view_shown_yaw;
        m_view_close_pitch = m_view_shown_pitch;
        m_view_land_yaw = roundf(m_view_shown_yaw / (2 * pi)) * 2 * pi;
        // Back to straight, turning the short way round.
        m_view_flip_to = roundf(m_view_yaw / (2 * pi)) * 2 * pi;
        m_view_flipping = true;
    } else if (accept && !ViewerLoading()) { // Over to the other side
        m_view_flip_to = (roundf(m_view_yaw / pi) + 1) * pi;
        m_view_flipping = true;
        m_view_vyaw = 0;
        UiSoundPlay(UI_SOUND_CHANGE);
    } else if (options) { // Straight, front on
        m_view_flip_to = roundf(m_view_yaw / (2 * pi)) * 2 * pi;
        m_view_flipping = true;
        m_view_vyaw = m_view_vpitch = 0;
        m_view_zoom_to = 1;
        UiSoundPlay(UI_SOUND_CHANGE);
    }
    (void)left, (void)right;
}

void DashboardScene::DrawViewer(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float dt = std::min(ImGui::GetIO().DeltaTime, 1 / 30.0f);
    const float pi = (float)M_PI;
    const Game &game = m_games[std::clamp(m_game, 0, (int)m_games.size() - 1)];

    // Opening (and closing, backwards): out of the row to the middle.
    float since = m_time - m_view_t;
    float p = m_view_closing ? 1 - std::clamp(since / 0.5f, 0.f, 1.f) :
                               std::clamp(since / 0.55f, 0.f, 1.f);
    if (m_view_closing && p <= 0) {
        m_view = false;
        m_view_e = 0;
        return;
    }
    // Opening: quick out of the row, settling in the middle. Closing: eased
    // at both ends, so it sets off gently and lands softly in its place.
    float e = m_view_closing ? p * p * p * (p * (p * 6 - 15) + 10) :
                               1 - (1 - p) * (1 - p) * (1 - p);
    m_view_e = e;

    // Turning: the sticks, then momentum, settling, floating.
    auto analog = [](ImGuiKey k) { return ImGui::GetKeyData(k)->AnalogValue; };
    float lx = analog(ImGuiKey_GamepadLStickRight) -
               analog(ImGuiKey_GamepadLStickLeft);
    float ly = analog(ImGuiKey_GamepadLStickDown) -
               analog(ImGuiKey_GamepadLStickUp);
    float ry = analog(ImGuiKey_GamepadRStickDown) -
               analog(ImGuiKey_GamepadRStickUp);
    bool loading = ViewerLoading();
    if (m_view_closing || loading) {
        lx = ly = 0; // Front on until the back and spine are in
    }
    if (m_view_closing) {
        ry = 0;
    }
    if (fabsf(lx) > 0.05f || fabsf(ly) > 0.05f) {
        m_view_flipping = false;
        m_view_vyaw = Approach(m_view_vyaw, lx * 3.4f, dt, 10);
        m_view_vpitch = Approach(m_view_vpitch, -ly * 2.4f, dt, 10);
    } else {
        m_view_vyaw *= expf(-1.6f * dt);   // Spinning on, slowing down
        m_view_vpitch *= expf(-4.0f * dt);
        if (fabsf(m_view_vpitch) < 0.3f) { // Tilting back level
            m_view_pitch = Approach(m_view_pitch, 0, dt, 1.8f);
        }
    }
    if (m_view_flipping) {
        m_view_yaw = Approach(m_view_yaw, m_view_flip_to, dt, 7);
        if (fabsf(m_view_yaw - m_view_flip_to) < 0.002f) {
            m_view_yaw = m_view_flip_to;
            m_view_flipping = false;
        }
    } else {
        m_view_yaw += m_view_vyaw * dt;
    }
    m_view_pitch = std::clamp(m_view_pitch + m_view_vpitch * dt, -1.1f, 1.1f);
    m_view_zoom_to = std::clamp(m_view_zoom_to - ry * dt * 1.3f, 0.7f, 1.55f);
    m_view_zoom = Approach(m_view_zoom, m_view_zoom_to, dt, 8);
    float idle = 1 - std::clamp((fabsf(m_view_vyaw) + fabsf(lx) + fabsf(ly)) * 2,
                                0.f, 1.f);
    float yaw, pitch;
    if (m_view_closing) {
        // Turning straight on the way, with a little swing, the floating
        // dying down: it lands flat, just as the row draws it.
        yaw = m_view_land_yaw + (m_view_close_yaw - m_view_land_yaw) * e +
              sinf(e * pi) * 0.25f;
        pitch = m_view_close_pitch * e;
        idle *= e;
    } else {
        yaw = m_view_yaw + idle * 0.07f * sinf(m_time * 0.9f) - (1 - e) * 0.7f;
        pitch = m_view_pitch + idle * 0.05f * sinf(m_time * 0.7f + 1);
        m_view_shown_yaw = yaw;
        m_view_shown_pitch = pitch;
    }

    // Behind it: the dashboard's own green, over the Games row.
    {
        float keep = m_alpha;
        m_alpha = keep * e;
        DrawBackground(s);
        m_alpha = keep;
    }
    XemuTexture front = GameCover(game);

    // Its size and place: from the cover's box in the row to the middle.
    float H = size.y * 0.62f * m_view_zoom, W = H * 0.71f, D = H * 0.075f;
    ImVec2 to(size.x / 2, size.y * 0.47f + idle * 8 * s * sinf(m_time * 1.3f));
    ImVec2 from((m_view_from0.x + m_view_from1.x) / 2,
                (m_view_from0.y + m_view_from1.y) / 2);
    float from_h = m_view_from1.y - m_view_from0.y;
    if (from_h <= 0) {
        from = to;
        from_h = H * 0.5f;
    }
    ImVec2 c(from.x + (to.x - from.x) * e, from.y + (to.y - from.y) * e);
    float k = (from_h + (H - from_h) * e) / H;
    H *= k, W *= k, D *= k;

    // Its shadow on the floor.
    dl->AddEllipseFilled(ImVec2(c.x, c.y + H * 0.6f),
                         ImVec2(W * (0.55f + 0.25f * fabsf(sinf(yaw))), H * 0.05f),
                         Rgba(0, 0, 0, a * 0.4f * e), 0, 40);

    // The box: turned (yaw about the upright, then pitch), in perspective.
    float cy_ = cosf(yaw), sy_ = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    float f = 2.6f * H;
    struct V3 { float x, y, z; };
    auto turn = [&](V3 v) {
        float x1 = v.x * cy_ + v.z * sy_, z1 = -v.x * sy_ + v.z * cy_;
        return V3{ x1, v.y * cp - z1 * sp, v.y * sp + z1 * cp };
    };
    auto project = [&](V3 v) {
        V3 t = turn(v);
        float kk = f / std::max(f + t.z, f * 0.2f);
        return ImVec2(c.x + t.x * kk, c.y + t.y * kk);
    };
    float hw = W / 2, hh = H / 2, hd = D / 2;
    // Light from the top left, in front.
    const V3 light = { -0.45f, -0.55f, -0.7f };
    struct Face {
        V3 n, corner[4]; // Normal; corners top left, top right, bottom right,
                         // bottom left as seen from outside
        int what;        // 0 front, 1 back, 2 spine, 3 plastic
    };
    const Face faces[] = {
        { { 0, 0, -1 }, { { -hw, -hh, -hd }, { hw, -hh, -hd }, { hw, hh, -hd }, { -hw, hh, -hd } }, 0 },
        { { 0, 0, 1 }, { { hw, -hh, hd }, { -hw, -hh, hd }, { -hw, hh, hd }, { hw, hh, hd } }, 1 },
        { { -1, 0, 0 }, { { -hw, -hh, hd }, { -hw, -hh, -hd }, { -hw, hh, -hd }, { -hw, hh, hd } }, 2 },
        { { 1, 0, 0 }, { { hw, -hh, -hd }, { hw, -hh, hd }, { hw, hh, hd }, { hw, hh, -hd } }, 3 },
        { { 0, -1, 0 }, { { -hw, -hh, hd }, { hw, -hh, hd }, { hw, -hh, -hd }, { -hw, -hh, -hd } }, 3 },
        { { 0, 1, 0 }, { { -hw, hh, -hd }, { hw, hh, -hd }, { hw, hh, hd }, { -hw, hh, hd } }, 3 },
    };
    XemuTexture back = GamePart(game, COVER_BACK);
    XemuTexture spine = GamePart(game, COVER_SPINE);
    float ap = a; // The row's case itself: solid all the way
    for (const Face &face : faces) {
        V3 n = turn(face.n);
        // Facing away as seen in perspective: its corners go round the
        // other way on screen (the normal alone is wrong near the edges).
        ImVec2 sc[4];
        for (int k = 0; k < 4; k++) {
            sc[k] = project(face.corner[k]);
        }
        float area = 0;
        for (int k = 0; k < 4; k++) {
            const ImVec2 &p0 = sc[k], &p1 = sc[(k + 1) % 4];
            area += p0.x * p1.y - p1.x * p0.y;
        }
        if (area <= 0.5f) {
            continue;
        }
        float lit = std::max(0.f, -(n.x * light.x + n.y * light.y + n.z * light.z)) /
                    sqrtf(light.x * light.x + light.y * light.y + light.z * light.z);
        float shade = face.what == 3 ? 0.35f + 0.65f * lit : 0.72f + 0.28f * lit;
        shade = 1 - (1 - shade) * e; // Lit as in the row, there
        ImU32 tint = Rgba((int)(255 * shade), (int)(255 * shade),
                          (int)(255 * shade), ap);
        XemuTexture tex = face.what == 0 ? front : face.what == 1 ? back :
                          face.what == 2 ? spine : 0;
        // In a grid, so the picture follows the perspective; on a dark
        // underlay of the whole face, so no seam between grid cells (or
        // faces) shows the background through.
        {
            ImU32 dark = Rgba(18, 20, 18, ap);
            ImU32 c4[4] = { dark, dark, dark, dark };
            ShadedQuad(dl, sc, c4); // No soft edge (that flickers)
        }
        // The same steps along every shared edge as the faces beside it
        // (6 across, 8 down, 1 deep), so the grids meet exactly: no cracks.
        bool flat = face.n.y != 0; // Top or bottom
        int cols = face.what < 2 || flat ? 6 : 1, rows = flat ? 1 : 8;
        // a + (b - a) * t, exact at t = 0 and 1 (so corners match).
        auto mix = [](const V3 &a, const V3 &b, float t) {
            return V3{ a.x * (1 - t) + b.x * t, a.y * (1 - t) + b.y * t,
                       a.z * (1 - t) + b.z * t };
        };
        auto at = [&](float u, float v) {
            const V3 *q = face.corner;
            return project(mix(mix(q[0], q[1], u), mix(q[3], q[2], u), v));
        };
        for (int j = 0; j < rows; j++) {
            for (int i = 0; i < cols; i++) {
                float u0 = (float)i / cols, u1 = (float)(i + 1) / cols;
                float v0 = (float)j / rows, v1 = (float)(j + 1) / rows;
                ImVec2 q0 = at(u0, v0), q1 = at(u1, v0), q2 = at(u1, v1),
                       q3 = at(u0, v1);
                if (tex) {
                    dl->AddImageQuad((ImTextureID)tex, q0, q1, q2, q3,
                                     ImVec2(u0, v0), ImVec2(u1, v0),
                                     ImVec2(u1, v1), ImVec2(u0, v1), tint);
                } else { // The case's dark edge, or no picture: dark
                    int k = face.what == 3 ? 46 : 22;
                    ImU32 col = Rgba((int)(k * shade), (int)((k + 4) * shade),
                                     (int)(k * shade), ap);
                    ImVec2 q[4] = { q0, q1, q2, q3 };
                    ImU32 c4[4] = { col, col, col, col };
                    ShadedQuad(dl, q, c4);
                }
            }
        }
        // A missing back: the disc's title image on it.
        if (face.what == 1 && !back) {
            XemuTexture image = GameImage(game);
            if (image) {
                dl->AddImageQuad((ImTextureID)image, at(0.3f, 0.33f),
                                 at(0.7f, 0.33f), at(0.7f, 0.61f), at(0.3f, 0.61f),
                                 ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1),
                                 ImVec2(0, 1), tint);
            }
        }
        // Gloss on the covers: a band of light sliding across as it turns.
        if (face.what < 2) {
            float g = (face.what == 0 ? 1 : -1) * sinf(yaw) * 1.6f + 0.5f;
            float strength = 0.22f * e * powf(std::max(0.f, -n.z), 2);
            for (int half = 0; half < 2; half++) {
                float ua = g + (half ? 0 : -0.16f), ub = g + (half ? 0.16f : 0);
                ua = std::clamp(ua, 0.f, 1.f), ub = std::clamp(ub, 0.f, 1.f);
                if (ub - ua < 0.002f) {
                    continue;
                }
                ImVec2 q[4] = { at(ua, 0), at(ub, 0), at(ub, 1), at(ua, 1) };
                ImU32 hi = White(ap * strength), lo = White(0);
                ImU32 cols4[4] = { half ? hi : lo, half ? lo : hi,
                                   half ? lo : hi, half ? hi : lo };
                ShadedQuad(dl, q, cols4);
            }
        }
        // Edges catching the light.
        ImVec2 e0 = at(0, 0), e1 = at(1, 0), e2 = at(1, 1), e3 = at(0, 1);
        dl->AddQuad(e0, e1, e2, e3, Rgba(255, 255, 255, ap * 0.12f * lit * e),
                    1.5f * s);
    }

    // Nothing but the case (the controls are in the hints along the bottom).
    ImFont *small = g_font_mgr.m_menu_font_small;
    // While the back cover and spine come in: a turning ring under it.
    if (loading) {
        ImVec2 rc(c.x, std::min(c.y + H * 0.62f + 30 * s, size.y - 250 * s));
        float turn = m_time * 5;
        dl->AddCircle(rc, 16 * s, White(a * e * 0.25f), 32, 3 * s);
        dl->PathArcTo(rc, 16 * s, turn, turn + 1.8f, 20);
        dl->PathStroke(White(a * e * 0.9f), 0, 3 * s);
        const char *msg = "Getting the back of the case";
        ImVec2 ms = TextSize(small, 26 * s, msg);
        Text(dl, small, 26 * s, ImVec2(rc.x - ms.x / 2, rc.y + 26 * s),
             White(a * e * 0.7f), msg);
    }
}

// The back cover or spine of the game in the viewer is on its way (asked
// for less than 10 s ago and not in yet).
bool DashboardScene::ViewerLoading()
{
    if (m_games.empty()) {
        return false;
    }
    const Game &game = m_games[std::clamp(m_game, 0, (int)m_games.size() - 1)];
    const GameArt &art = Art(game.path);
    bool waiting = (!art.back && art.back_tried) || (!art.spine && art.spine_tried);
    return waiting && ImGui::GetTime() - art.parts_asked < 10;
}

// Games: XPSemu's showcase. The cases stand in a row on a glossy floor;
// the selection is big, risen and floating a little, and a glint of light
// crosses it once it settles. Moving, the row glides quickly on a spring
// and every case swings round in 3D with the speed, settling flat again as
// it stops (each side case keeps a slight turn towards the middle). Dust
// drifts in front of and behind the row (at different depths, so it moves
// past at different speeds), and on opening the cases rise in from the
// middle out.
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
    float cx = size.x / 2, cy = 500 * s;
    const float center_h = 500 * s, aspect = 0.71f;
    const float cw = center_h * aspect; // The selected case's width
    const float gap = 24 * s;
    const float floor_y = cy + center_h / 2; // The cases stand on it
    const int reach = 6; // Cases drawn either side
    const float side = 0.58f; // A side case's size, of the selection's

    // Size by distance from the selection (d fractional while scrolling):
    // only the one passing through the middle changes, the rest keep theirs.
    auto scale_at = [&](float d) {
        return side + (1 - side) * expf(-d * d * 4);
    };
    // Where a case's middle sits: even steps, plus the selection's extra
    // half width eased in, so neighbours never touch while scrolling.
    auto x_at = [&](float d) {
        float ad = fabsf(d);
        float x = ad * (cw * side + gap) + cw * (1 - side) / 2 * tanhf(ad * 2.5f);
        return d < 0 ? -x : x;
    };

    float settled = 1 - std::clamp(fabsf(m_game_anim - m_game) * 3, 0.f, 1.f);
    // The swing: every case turns about its upright axis with the speed.
    float swing = std::clamp(-m_game_vel * 0.07f, -0.6f, 0.6f);

    // A glint of light crosses the selection once it settles, then every
    // few seconds.
    if (m_glint_game != m_game) {
        m_glint_game = m_game;
        m_glint_start = m_time + 0.3f;
    }
    if (settled > 0.99f && m_time > m_glint_start + 0.2f) {
        GamePart(m_games[m_game], COVER_BACK); // Ready for the viewer
        GamePart(m_games[m_game], COVER_SPINE);
    }


    float sel_lift = 34 * s + 5 * s * sinf(m_time * 1.7f) * settled;

    // Dust: slow motes rising and swaying, at two depths. The row's scroll
    // moves them sideways (the near ones faster), which reads as depth.
    auto dust = [&](bool front) {
        int count = front ? 14 : 30;
        float depth = front ? 0.55f : 0.14f;
        float span = size.x + 300 * s;
        const int r = 170, g = 235, b = 70;
        for (int i = 0; i < count; i++) {
            uint32_t h = (uint32_t)(i * 2654435761u + (front ? 97 : 13));
            float u = (h & 0xffff) / 65535.0f;
            float v = ((h >> 16) & 0xffff) / 65535.0f;
            float speed = (front ? 22 : 12) * s * (0.6f + v);
            float x = u * span - m_game_anim * cw * depth +
                      sinf(m_time * (0.3f + v * 0.4f) + i) * 30 * s;
            x = fmodf(x, span);
            if (x < 0) {
                x += span;
            }
            x -= 150 * s;
            float y = fmodf(v * size.y - m_time * speed, size.y);
            if (y < 0) {
                y += size.y;
            }
            float fade = std::clamp(std::min(y - 120 * s, size.y - y) /
                                        (160 * s), 0.f, 1.f);
            float tw = 0.55f + 0.45f * sinf(m_time * (1 + v * 2) + i * 1.7f);
            float al = a * fade * tw * (front ? 0.35f : 0.5f);
            float rad = (front ? 4 + u * 5 : 1.5f + v * 2.5f) * s;
            dl->AddCircleFilled(ImVec2(x, y), rad * 2.6f,
                                Rgba(r, g, b, al * 0.18f), 16);
            dl->AddCircleFilled(ImVec2(x, y), rad, Rgba(r, g, b, al), 12);
        }
    };
    dust(false);

    // A soft green halo behind the selection.
    for (int i = 4; i >= 1; i--) {
        float g = i * 26 * s;
        float hy = floor_y - center_h / 2 - sel_lift;
        dl->AddRectFilled(ImVec2(cx - cw / 2 - g, hy - center_h / 2 - g),
                          ImVec2(cx + cw / 2 + g, hy + center_h / 2 + g),
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

    // The viewer back in its place: done before the row is drawn, so the
    // case is in it this very frame (not a frame with neither).
    if (m_view && m_view_closing && m_time - m_view_t >= 0.5f) {
        m_view = false;
        m_view_e = 0;
    }
    ImVec2 sel0, sel1; // The selection's box, for its outline and glint
    for (int i : order) {
        float d = i - m_game_anim;
        float ad = fabsf(d);
        float sc = scale_at(d);
        float w = cw * sc, h = center_h * sc;
        float near = expf(-d * d * 4); // 1 at the selection
        float dim = 1 - 0.32f * (1 - near) - 0.04f * std::min(ad, 5.f);

        // Rising in from below on opening, from the middle out.
        float p = std::clamp((m_page_anim - 0.07f * std::min(ad, 6.f)) / 0.55f,
                             0.f, 1.f);
        p = 1 - (1 - p) * (1 - p) * (1 - p);
        float edge = ad > reach - 1 ? reach - ad : 1;
        float ca = a * std::clamp(edge, 0.f, 1.f) * p;
        if (ca <= 0.004f) {
            continue;
        }

        float bottom = floor_y - near * sel_lift + (1 - p) * 160 * s;
        // Turned a little towards the middle, plus the swing.
        float turn = std::clamp(d, -1.f, 1.f) * 0.22f + swing;
        Card card = Card3D(ImVec2(cx + x_at(d), bottom - h / 2), w, h, turn, 0,
                           0, 2.6f * cw);
        ImVec2 tl = card.p[0], tr = card.p[1], br = card.p[2], bl = card.p[3];
        float hl = bl.y - tl.y, hr = br.y - tr.y;

        // The selection out in the Box Art Viewer: its place stays empty,
        // its reflection fading back in as it returns.
        bool viewed = m_view && i == m_game;
        XemuTexture cover = viewed ? GameCover(m_games[i]) :
                                     DrawCase(m_games[i], tl, tr, br, bl, ca, dim);
        if (i == m_game) {
            sel0 = ImVec2(std::min(tl.x, bl.x), std::min(tl.y, tr.y));
            sel1 = ImVec2(std::max(tr.x, br.x), std::max(bl.y, br.y));
        }

        // The reflection: the bottom of the case, flipped, on the glossy
        // floor, fading out.
        {
            float ra = viewed ? ca * (1 - m_view_e) : ca;
            float rk = 0.3f; // Of the case's height
            ImVec2 rtl(bl.x, bl.y + 4 * s), rtr(br.x, br.y + 4 * s);
            ImVec2 rbl(bl.x, rtl.y + hl * rk), rbr(br.x, rtr.y + hr * rk);
            int v0 = dl->VtxBuffer.Size;
            if (cover) {
                dl->AddImageQuad((ImTextureID)cover, rtl, rtr, rbr, rbl,
                                 ImVec2(0, 1), ImVec2(1, 1), ImVec2(1, 1 - rk),
                                 ImVec2(0, 1 - rk), White(ra * 0.34f * dim));
            } else {
                dl->AddQuadFilled(rtl, rtr, rbr, rbl,
                                  Rgba((int)(30 * dim), (int)(90 * dim),
                                       (int)(20 * dim), ra * 0.34f));
            }
            FadeDown(dl, v0, std::min(rtl.y, rtr.y), std::max(rbl.y, rbr.y));
            // A thin sheen where the case meets the floor.
            dl->AddLine(rtl, rtr, White(ra * 0.25f * dim), 1.5f * s);
        }
    }
    m_sel0 = sel0;
    m_sel1 = sel1;
    dust(true);

    // The selection: its outline, a glint of light and a few sparkles,
    // once it has settled.
    const Game &game = m_games[m_game];
    float focus_alpha = a * settled * std::clamp(m_page_anim * 2 - 1, 0.f, 1.f);
    if (focus_alpha > 0.01f) {
        Focus(dl, sel0, sel1, s, m_time, focus_alpha);

        float q = fmodf(std::max(m_time - m_glint_start, 0.f), 6.f) / 0.9f;
        if (m_time >= m_glint_start && q < 1) {
            float bw = (sel1.x - sel0.x) * 0.28f, sh = sel1.y - sel0.y;
            float gx = sel0.x - bw - sh * 0.35f +
                       q * (sel1.x - sel0.x + bw * 2 + sh * 0.35f);
            float ga = focus_alpha * 0.22f * sinf(q * (float)M_PI);
            dl->PushClipRect(sel0, sel1, true);
            dl->AddQuadFilled(ImVec2(gx + sh * 0.35f, sel0.y),
                              ImVec2(gx + sh * 0.35f + bw, sel0.y),
                              ImVec2(gx + bw, sel1.y), ImVec2(gx, sel1.y),
                              White(ga));
            dl->PopClipRect();
        }

        // Small glints wink around the selected cover.
        float fw = sel1.x - sel0.x, fh = sel1.y - sel0.y;
        const ImVec2 sparkle_pos[] = {
            ImVec2(sel0.x - 10 * s, sel0.y + fh * 0.15f),
            ImVec2(sel1.x + 8 * s, sel0.y + fh * 0.34f),
            ImVec2(sel0.x - 6 * s, sel1.y - fh * 0.24f),
            ImVec2(sel1.x + 4 * s, sel1.y - fh * 0.11f),
            ImVec2(sel0.x + fw * 0.6f, sel0.y - 10 * s),
        };
        for (int i = 0; i < (int)(sizeof(sparkle_pos) /
                                  sizeof(sparkle_pos[0])); i++) {
            float pulse = 0.5f + 0.5f * sinf(m_time * 2.8f + i * 2.2f);
            float k = std::clamp((pulse - 0.55f) / 0.45f, 0.f, 1.f);
            if (k <= 0) {
                continue;
            }
            ImVec2 c = sparkle_pos[i];
            float r = (3.5f + 5.5f * k) * s;
            ImU32 color = White(focus_alpha * k * 0.9f);
            dl->AddCircleFilled(c, r * 1.8f, Lime(focus_alpha * k * 0.18f),
                                12);
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), color,
                        1.8f * s);
            dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), color,
                        1.8f * s);
            dl->AddLine(ImVec2(c.x - r * 0.55f, c.y - r * 0.55f),
                        ImVec2(c.x + r * 0.55f, c.y + r * 0.55f), color,
                        1.2f * s);
        }
    }

    // The name and details fade and drop a little while scrolling, and
    // come back up for the new selection.
    float na = 0.15f + 0.85f * settled;
    a *= na;
    float fh = center_h;
    float ty = cy + fh / 2 + 44 * s + 14 * s * (1 - settled);
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
    for (int o = GO_SCALE + 1; o < GO__COUNT; o++) { // Not resolution
        if (o == GO_FILTER || o == GO_DSP) {
            continue; // Main settings only
        }
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
    if (m_view) {
        DrawViewer(s, m_alpha * m_page_anim);
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

//
// Side art (display.ui.blades_sides): where a 4:3 game leaves the sides of
// the screen empty, panels after the Xbox 360's "Blades" dashboard instead
// of black: glossy orange, a cream frame and a silver blade (original art
// in that style: ps5/art/make-blades.py). Your own instead:
// /data/xemu/blades-left.png and blades-right.png (blades-green-*.png for
// green). Only while a game runs,
// with the dashboard and the launch screen gone. Called by RenderFramebuffer
// (vk-helpers.cc) with the game's box.
//

#include "blades-art.h"

static XemuTexture g_blades[2][2]; // By style (green, orange), side
static bool g_blades_tried[2];

static XemuTexture BladesTexture(const char *file, const unsigned char *data,
                                 int size)
{
    int w, h, channels;
    std::string path = std::string(xemu_settings_get_base_path()) + file;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (pixels) {
        fprintf(stderr, "XPSemu: side art from %s\n", path.c_str());
    } else {
        pixels = stbi_load_from_memory(data, size, &w, &h, &channels, 4);
    }
    if (!pixels) {
        return 0;
    }
    XemuTexture tex = xemu_vk_texture_create(pixels, w, h, 4);
    stbi_image_free(pixels);
    return tex;
}

void DrawBladeSides(ImDrawList *dl, ImVec2 g0, ImVec2 g1)
{
    ImVec2 size = ImGui::GetIO().DisplaySize;
    float s = size.y / 1080.0f;
    if (!g_config.display.ui.blades_sides || g0.x < 40 * s ||
        !GameAnyRunning() || g_dashboard.Visible() || g_curtain.active) {
        return;
    }
    int style = std::clamp(g_config.display.ui.blades_style, 0, 1);
    if (!g_blades_tried[style]) {
        g_blades_tried[style] = true;
        g_blades[style][0] =
            style ? BladesTexture("blades-left.png", kBladesLeft,
                                  sizeof(kBladesLeft)) :
                    BladesTexture("blades-green-left.png", kBladesGreenLeft,
                                  sizeof(kBladesGreenLeft));
        g_blades[style][1] =
            style ? BladesTexture("blades-right.png", kBladesRight,
                                  sizeof(kBladesRight)) :
                    BladesTexture("blades-green-right.png", kBladesGreenRight,
                                  sizeof(kBladesGreenRight));
    }
    if (g_blades[style][0]) {
        dl->AddImage((ImTextureID)g_blades[style][0], ImVec2(0, 0),
                     ImVec2(g0.x, size.y));
    }
    if (g_blades[style][1]) {
        dl->AddImage((ImTextureID)g_blades[style][1], ImVec2(g1.x, 0), size);
    }
    // A soft shadow where they meet the game.
    float sw = 18 * s;
    dl->AddRectFilledMultiColor(ImVec2(g0.x - sw, 0), ImVec2(g0.x, size.y),
                                Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0.5f),
                                Rgba(0, 0, 0, 0.5f), Rgba(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(g1.x, 0), ImVec2(g1.x + sw, size.y),
                                Rgba(0, 0, 0, 0.5f), Rgba(0, 0, 0, 0),
                                Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0.5f));
}

// Settings' category icons, in white: a screen, a speaker, sliders, a gear.
static void DrawCategoryIcon(ImDrawList *dl, ImVec2 c, float r, int cat,
                             float a, bool selected)
{
    float s = ImGui::GetIO().DisplaySize.y / 1080.0f;
    // White with a soft shadow; on the selected (lime) bar, dark like its
    // text.
    for (int pass = selected ? 1 : 0; pass < 2; pass++) {
        ImVec2 o = pass ? c : ImVec2(c.x + 2 * s, c.y + 2 * s);
        ImU32 col = !pass ? Rgba(0, 0, 0, a * 0.35f) :
                    selected ? Ink(a) : White(a);
        float t = 3.2f * s;
        switch (cat) {
        case CAT_VIDEO: // A screen on a stand
            dl->AddRect(ImVec2(o.x - r, o.y - r * 0.72f),
                        ImVec2(o.x + r, o.y + r * 0.5f), col, 4 * s, 0, t);
            dl->AddLine(ImVec2(o.x, o.y + r * 0.5f), ImVec2(o.x, o.y + r * 0.82f),
                        col, t);
            dl->AddLine(ImVec2(o.x - r * 0.5f, o.y + r * 0.86f),
                        ImVec2(o.x + r * 0.5f, o.y + r * 0.86f), col, t);
            break;
        case CAT_SOUND: { // A speaker and two waves
            ImVec2 body[] = { ImVec2(o.x - r, o.y - r * 0.32f),
                              ImVec2(o.x - r * 0.55f, o.y - r * 0.32f),
                              ImVec2(o.x, o.y - r * 0.8f),
                              ImVec2(o.x, o.y + r * 0.8f),
                              ImVec2(o.x - r * 0.55f, o.y + r * 0.32f),
                              ImVec2(o.x - r, o.y + r * 0.32f) };
            dl->AddConvexPolyFilled(body, 6, col);
            for (int k = 1; k <= 2; k++) {
                dl->PathArcTo(ImVec2(o.x + r * 0.05f, o.y), r * (0.38f + 0.4f * k),
                              -0.8f, 0.8f, 16);
                dl->PathStroke(col, 0, t);
            }
            break;
        }
        case CAT_INTERFACE: // Three sliders
            for (int k = 0; k < 3; k++) {
                float yy = o.y + (k - 1) * r * 0.66f;
                float knob = o.x + r * (k == 0 ? 0.35f : k == 1 ? -0.4f : 0.1f);
                dl->AddLine(ImVec2(o.x - r, yy), ImVec2(o.x + r, yy), col, t);
                dl->AddCircleFilled(ImVec2(knob, yy), r * 0.22f, col, 16);
            }
            break;
        case CAT_CONTROLLER: { // A controller
            dl->AddEllipse(ImVec2(o.x - r * 0.55f, o.y + r * 0.1f),
                           ImVec2(r * 0.45f, r * 0.62f), col, 0.5f, 24, t);
            dl->AddEllipse(ImVec2(o.x + r * 0.55f, o.y + r * 0.1f),
                           ImVec2(r * 0.45f, r * 0.62f), col, -0.5f, 24, t);
            dl->AddRect(ImVec2(o.x - r * 0.8f, o.y - r * 0.45f),
                        ImVec2(o.x + r * 0.8f, o.y + r * 0.25f), col, r * 0.3f, 0, t);
            dl->AddCircleFilled(ImVec2(o.x + r * 0.45f, o.y - r * 0.12f),
                                r * 0.12f, col, 10);
            dl->AddLine(ImVec2(o.x - r * 0.62f, o.y - r * 0.12f),
                        ImVec2(o.x - r * 0.28f, o.y - r * 0.12f), col, t);
            break;
        }
        case CAT_PATCHES: { // A patch: a plaster with a cross on it
            dl->AddRect(ImVec2(o.x - r, o.y - r * 0.5f), ImVec2(o.x + r, o.y + r * 0.5f),
                        col, r * 0.45f, 0, t);
            dl->AddLine(ImVec2(o.x, o.y - r * 0.25f), ImVec2(o.x, o.y + r * 0.25f),
                        col, t);
            dl->AddLine(ImVec2(o.x - r * 0.25f, o.y), ImVec2(o.x + r * 0.25f, o.y),
                        col, t);
            break;
        }
        default: { // A gear
            const int teeth = 8;
            for (int k = 0; k < teeth * 4; k++) {
                float ang = (k / 4 + (k % 4) * 0.25f - 0.125f) * 2 *
                            (float)M_PI / teeth;
                float rad = (k % 4 == 1 || k % 4 == 2) ? r : r * 0.74f;
                dl->PathLineTo(ImVec2(o.x + cosf(ang) * rad, o.y + sinf(ang) * rad));
            }
            dl->PathStroke(col, ImDrawFlags_Closed, t);
            dl->AddCircle(o, r * 0.3f, col, 20, t);
            break;
        }
        }
    }
}

//
// Controller setup (Settings > Controller): an original Xbox controller in
// the middle, its buttons called out either side with the DualSense button
// each comes from. X on one, then press the DualSense button you want for
// it (whatever had that button gets this one's old one); Triangle puts the
// standard layout back.
//

// The page's rows: the Xbox controls (left column, right column).
static const int kPadLeft[] = { XB_LT, XB_WHITE, XB_LSTICK, XB_UP, XB_LEFT,
                                XB_RIGHT, XB_DOWN, XB_BACK };
static const int kPadRight[] = { XB_RT, XB_BLACK, XB_Y, XB_X, XB_B, XB_A,
                                 XB_RSTICK, XB_START };
enum { PAD_ROWS_CONTROLS = 16, PAD_ROWS = 16 };

static int PadRowControl(int row)
{
    return row < 8 ? kPadLeft[row] : row < 16 ? kPadRight[row - 8] : -1;
}

static const char *XboxControlName(int x)
{
    static const char *const names[XB__COUNT] = {
        "A", "B", "X", "Y", "Black", "White", "Back", "Start", "Left stick",
        "Right stick", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
        "Left trigger", "Right trigger",
    };
    return x >= 0 && x < XB__COUNT ? names[x] : "";
}

extern "C" int g_xemu_ps5_pad_hold;

void DashboardScene::PadInput(bool accept, bool left, bool right, bool up,
                              bool down, bool options)
{
    if (up) m_pad_sel = (m_pad_sel + PAD_ROWS - 1) % PAD_ROWS;
    if (down) m_pad_sel = (m_pad_sel + 1) % PAD_ROWS;
    if (m_pad_sel < PAD_ROWS_CONTROLS) {
        if (left || right) { // The other column, same height
            m_pad_sel = (m_pad_sel + 8) % 16;
        }
        if (accept) { // Wait for the DualSense button to use
            m_pad_capture = true;
            m_pad_capture_t = m_time;
            m_pad_released = false;
        }
    }
    if (options) {
        xemu_ps5_map_reset();
        UiSoundPlay(UI_SOUND_SELECT);
        xemu_queue_notification("Controller: standard layout back");
    }
}

// While waiting for a button: the first one pressed (after letting go of
// the one that started it) is the new one; none in 6 s, nothing changes.
void DashboardScene::PadCapture()
{
    int pressed = xemu_ps5_pad_pressed_source();
    if (!m_pad_released) {
        m_pad_released = pressed < 0;
    } else if (pressed >= 0) {
        xemu_ps5_map_set(PadRowControl(m_pad_sel), pressed);
        m_pad_capture = false;
        m_pad_done_t = m_time;
        UiSoundPlay(UI_SOUND_CHANGE);
        m_prev_buttons = g_input_mgr.CombinedButtons(); // Not a press here
    }
    if (m_pad_capture && m_time - m_pad_capture_t > 6) {
        m_pad_capture = false;
        UiSoundPlay(UI_SOUND_BACK);
    }
}

void DashboardScene::DrawPadPage(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);
    ImVec2 c(size.x / 2 + slide, 560 * s);
    auto at = [&](float x, float y) { return ImVec2(c.x + x * s, c.y + y * s); };

    // The controller: its outline first (bigger, lime), then the body.
    for (int pass = 0; pass < 2; pass++) {
        float g = pass ? 0 : 4;
        ImU32 col = pass ? Rgba(22, 30, 22, a) : Lime(a * 0.75f);
        dl->AddRectFilled(at(-250 - g, -118 - g), at(250 + g, 72 + g), col,
                          (70 + g) * s);
        for (int side = -1; side <= 1; side += 2) {
            dl->AddEllipseFilled(at(side * 188, 92), ImVec2((96 + g) * s, (152 + g) * s),
                                 col, side * -0.38f, 48);
            dl->AddRectFilled(at(side * 170 - 52 - g, -168 - g),
                              at(side * 170 + 52 + g, -110 + g), col, 14 * s);
        }
    }
    // A soft sheen along the top.
    dl->AddRectFilledMultiColor(at(-200, -112), at(200, -60), White(a * 0.06f),
                                White(a * 0.06f), White(0), White(0));
    // The jewel (no logo: XPSemu green).
    dl->AddCircleFilled(at(0, -78), 30 * s, Rgba(90, 200, 30, a * 0.25f), 32);
    dl->AddCircleFilled(at(0, -78), 22 * s, Rgba(70, 180, 30, a), 32);
    dl->AddCircleFilled(at(-6, -84), 7 * s, White(a * 0.6f), 16);
    // Sticks, d-pad, face buttons, black and white, back and start.
    for (ImVec2 st : { ImVec2(-155, -30), ImVec2(75, 55) }) {
        dl->AddCircleFilled(at(st.x, st.y), 44 * s, Rgba(10, 14, 10, a), 32);
        dl->AddCircleFilled(at(st.x, st.y), 31 * s, Rgba(48, 56, 48, a), 32);
        dl->AddCircle(at(st.x, st.y), 31 * s, Line(a), 32, 2 * s);
    }
    dl->AddRectFilled(at(-108, 44), at(-62, 66), Rgba(48, 56, 48, a), 4 * s);
    dl->AddRectFilled(at(-96, 32), at(-74, 78), Rgba(48, 56, 48, a), 4 * s);
    const struct { float x, y; int r, g, b; } face[] = {
        { 155, 5, 70, 190, 50 },   // A
        { 190, -30, 210, 50, 40 }, // B
        { 120, -30, 50, 110, 220 }, // X
        { 155, -65, 230, 200, 40 }, // Y
    };
    for (const auto &f : face) {
        dl->AddCircleFilled(at(f.x, f.y), 17 * s, Rgba(f.r, f.g, f.b, a), 24);
        dl->AddCircleFilled(at(f.x - 5, f.y - 5), 5 * s, White(a * 0.45f), 12);
    }
    dl->AddCircleFilled(at(214, 34), 11 * s, Rgba(8, 8, 8, a), 20);
    dl->AddCircle(at(214, 34), 11 * s, Line(a), 20, 1.5f * s);
    dl->AddCircleFilled(at(186, 60), 11 * s, Rgba(235, 235, 235, a), 20);
    dl->AddRectFilled(at(-52, -20), at(-28, -8), Rgba(48, 56, 48, a), 6 * s);
    dl->AddRectFilled(at(28, -20), at(52, -8), Rgba(48, 56, 48, a), 6 * s);

    // Where each control's callout points.
    auto anchor = [&](int x) {
        switch (x) {
        case XB_A: return at(155, 5);      case XB_B: return at(190, -30);
        case XB_X: return at(120, -30);    case XB_Y: return at(155, -65);
        case XB_BLACK: return at(214, 34); case XB_WHITE: return at(186, 60);
        case XB_BACK: return at(-40, -14); case XB_START: return at(40, -14);
        case XB_LSTICK: return at(-155, -30); case XB_RSTICK: return at(75, 55);
        case XB_UP: return at(-85, 36);    case XB_DOWN: return at(-85, 74);
        case XB_LEFT: return at(-104, 55); case XB_RIGHT: return at(-66, 55);
        case XB_LT: return at(-170, -150); default: return at(170, -150);
        }
    };

    // Which Xbox controls a game would get pressed right now: the same
    // state the game reads (the DualSense, remapped).
    uint32_t live = 0;
    if (ControllerState *pad = xemu_input_get_bound(0)) {
        static const uint32_t bits[XB_LT] = {
            CONTROLLER_BUTTON_A,      CONTROLLER_BUTTON_B,
            CONTROLLER_BUTTON_X,      CONTROLLER_BUTTON_Y,
            CONTROLLER_BUTTON_BLACK,  CONTROLLER_BUTTON_WHITE,
            CONTROLLER_BUTTON_BACK,   CONTROLLER_BUTTON_START,
            CONTROLLER_BUTTON_LSTICK, CONTROLLER_BUTTON_RSTICK,
            CONTROLLER_BUTTON_DPAD_UP, CONTROLLER_BUTTON_DPAD_DOWN,
            CONTROLLER_BUTTON_DPAD_LEFT, CONTROLLER_BUTTON_DPAD_RIGHT,
        };
        for (int x = 0; x < XB_LT; x++) {
            if (pad->buttons & bits[x]) {
                live |= 1u << x;
            }
        }
        if (pad->axis[CONTROLLER_AXIS_LTRIG] > 16000) live |= 1u << XB_LT;
        if (pad->axis[CONTROLLER_AXIS_RTRIG] > 16000) live |= 1u << XB_RT;
    }

    // The callouts: both columns.
    float bw = 400 * s, bh = 56 * s, gap = 70 * s, y0 = 232 * s;
    for (int row = 0; row < PAD_ROWS_CONTROLS; row++) {
        bool right = row >= 8;
        int x = PadRowControl(row);
        float bx = right ? size.x - 140 * s - bw + slide : 140 * s + slide;
        float by = y0 + (row % 8) * gap;
        bool selected = row == m_pad_sel;
        ImVec2 p0(bx, by), p1(bx + bw, by + bh);
        // The line to the control, dashed.
        ImVec2 from(right ? p0.x : p1.x, by + bh / 2), to = anchor(x);
        float len = sqrtf((to.x - from.x) * (to.x - from.x) +
                          (to.y - from.y) * (to.y - from.y));
        for (float d = 0; d < len; d += 12 * s) {
            float d1 = std::min(d + 6 * s, len);
            dl->AddLine(ImVec2(from.x + (to.x - from.x) * d / len,
                               from.y + (to.y - from.y) * d / len),
                        ImVec2(from.x + (to.x - from.x) * d1 / len,
                               from.y + (to.y - from.y) * d1 / len),
                        selected ? Lime(a) : Line(a * 0.6f), 1.5f * s);
        }
        dl->AddCircleFilled(to, 4 * s, selected ? Lime(a) : Line(a * 0.8f), 12);
        // Pressed now, as a game gets it: lit.
        if (live & (1u << x)) {
            dl->AddCircleFilled(to, 22 * s, Lime(a * 0.35f), 24);
            dl->AddCircleFilled(to, 9 * s, White(a), 16);
            dl->AddRect(p0, p1, Lime(a), 10 * s, 0, 3 * s);
        }
        Bar(dl, p0, p1, s, selected, a);
        Text(dl, small, 30 * s, ImVec2(bx + 26 * s, by + 12 * s),
             selected ? Ink(a) : Label(a), XboxControlName(x));
        bool waiting = selected && m_pad_capture;
        bool just = selected && m_time - m_pad_done_t < 0.6f;
        const char *src = waiting ? "Press a button..." :
                                    xemu_ps5_source_name(xemu_ps5_map_get(x));
        ImVec2 ss = TextSize(small, 30 * s, src);
        float blink = waiting ? 0.55f + 0.45f * sinf(m_time * 8) : 1;
        Text(dl, small, 30 * s, ImVec2(bx + bw - ss.x - 26 * s, by + 12 * s),
             selected ? Ink(a * blink) : (just ? Lime(a) : White(a * 0.9f)), src);
    }

    // Focus, and what the selection does.
    bool right_col = m_pad_sel >= 8;
    float fx = right_col ? size.x - 140 * s - bw + slide : 140 * s + slide;
    ImVec2 f0(fx, y0 + (m_pad_sel % 8) * gap), f1(f0.x + bw, f0.y + bh);
    Focus(dl, f0, f1, s, m_time, a);
    const char *help =
        m_pad_capture ? "Press the DualSense button you want for it (6 s)." :
            "X to change it, Triangle for the standard layout. Press "
            "buttons to see what a game gets.";
    ImVec2 hs = TextSize(small, 30 * s, help);
    Text(dl, small, 30 * s, ImVec2(size.x / 2 - hs.x / 2 + slide, 910 * s),
         Label(a), help);
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
    auto on_off = [](bool on) { return on ? "On" : "Off"; };
    values[SR_SIDES] = !g_config.display.ui.blades_sides ? "Off" :
                       g_config.display.ui.blades_style == 1 ? "Orange" :
                                                            "Green";
    values[SR_SOUNDS] = on_off(g_config.display.ui.menu_sounds);
    values[SR_MUSIC] = on_off(g_config.display.ui.menu_music);
    values[SR_COVERS] = on_off(g_config.display.ui.download_covers);
    values[SR_BOOT] = on_off(g_config.display.ui.boot_logo);
    values[SR_XBOX_BOOT] = on_off(!g_config.general.skip_boot_anim);
    bool scale_locked = false; // Applies at the next start instead
    if (GameGlobal(GO_SCALE) != (int)nv2a_get_surface_scale_factor()) {
        values[SR_SCALE] += "  (next start)";
    }
    const struct {
        const char *name, *help;
    } rows[SR__COUNT] = {
        { "Resolution", "Higher is sharper (no slower on the PS5). Every game "
                        "uses it; a change applies when XPSemu next starts." },
        { "Screen shape",
          "Most games are 4:3. For a game in widescreen mode (Xbox setting), "
          "give it 16:9 with Triangle on it." },
        { "Picture fit", "Stretch fills the whole screen." },
        { "Smoothing", "Off keeps the pixels sharp." },
        { "Side art",
          "Panels after the Xbox 360's Blades dashboard beside 4:3 games, "
          "instead of black bars: green or orange." },
        { "Volume", "How loud the games are." },
        { "Menu sounds",
          "The dashboard's ticks and chimes. Your own: WAVs in "
          "/data/xemu/sounds." },
        { "Music",
          "Music in the dashboard, fading out when a game plays. Your own: "
          "/data/xemu/sounds/music.wav." },
        { "Performance overlay",
          "Frame rate, CPU and memory in the corner while you play." },
        { "Download covers",
          "Covers for games without one, from xdb (the xemu project's Xbox "
          "archive) by title ID. Needs the internet; each one once." },
        { "XPSemu startup", "XPSemu's logo and sound when the app starts." },
        { "Original Xbox startup",
          "The Xbox's own boot animation, after XPSemu's, from the next time "
          "XPSemu starts (games always start straight away)." },
    };

    float x = 280 * s + slide, w = 1400 * s;
    if (m_settings_cat < 0) {
        // The categories, each with what's in it.
        float h = 86 * s, gap = 102 * s, y0 = 236 * s;
        for (int i = 0; i < CAT__COUNT; i++) {
            float y = y0 + i * gap;
            bool selected = i == m_cat_sel;
            Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, a);
            DrawCategoryIcon(dl, ImVec2(x + 66 * s, y + h / 2), 22 * s, i, a,
                             selected);
            Text(dl, font, 46 * s, ImVec2(x + 118 * s, y + 17 * s),
                 selected ? Ink(a) : Label(a), kCategories[i].name);
            std::string summary;
            for (int k = 0; k < kCategories[i].count && k < 4; k++) {
                int row = kCategories[i].rows[k];
                summary += (summary.empty() ? "" : "   ") + values[row];
            }
            if (i == CAT_ADVANCED) {
                summary = "Memory, video output, audio, performance";
            } else if (i == CAT_CONTROLLER) {
                summary = "Your button layout";
            } else if (i == CAT_PATCHES) {
                summary = "All your games' patches";
            }
            ImVec2 ss = TextSize(small, 30 * s, summary.c_str());
            Text(dl, small, 30 * s, ImVec2(x + w - ss.x - 60 * s, y + 27 * s),
                 selected ? Ink(a * 0.8f) : White(a * 0.75f), summary.c_str());
        }
        Focus(dl, ImVec2(x, y0 + m_cat_sel * gap),
              ImVec2(x + w, y0 + m_cat_sel * gap + h), s, m_time, a);
        TextFit(dl, small, 32 * s, ImVec2(x + 10 * s, y0 + CAT__COUNT * gap + 10 * s),
                Label(a), kCategories[m_cat_sel].help, w);
        return;
    }

    // A category's rows.
    const auto &cat = kCategories[m_settings_cat];
    float h = 70 * s, gap = 84 * s, y0 = 236 * s;
    int at = 0;
    for (int i = 0; i < cat.count; i++) {
        int row = cat.rows[i];
        if (row == m_setting) {
            at = i;
        }
        float y = y0 + i * gap;
        bool selected = row == m_setting;
        bool locked = row == SR_SCALE && scale_locked;
        float ra = locked ? a * 0.55f : a;
        Bar(dl, ImVec2(x, y), ImVec2(x + w, y + h), s, selected, ra);
        Text(dl, font, 42 * s, ImVec2(x + 40 * s, y + 12 * s),
             selected ? Ink(a) : Label(ra), rows[row].name);
        std::string value = values[row] + (locked ? "  (locked)" : "");
        if (!value.empty()) {
            char text[96];
            snprintf(text, sizeof(text), selected && !locked ? "<  %s  >" : "%s",
                     value.c_str());
            ImVec2 vs = TextSize(font, 42 * s, text);
            Text(dl, font, 42 * s, ImVec2(x + w - vs.x - 50 * s, y + 12 * s),
                 selected ? Ink(a) : White(ra * 0.9f), text);
        }
    }
    Focus(dl, ImVec2(x, y0 + at * gap), ImVec2(x + w, y0 + at * gap + h), s,
          m_time, a);

    // The selection's explanation, under the list.
    const char *help = rows[m_setting].help;
    if (m_setting == SR_SCALE && scale_locked) {
        help = "Locked while a game runs. Change it before starting one "
               "(or after the Xbox Dashboard button).";
    }
    TextFit(dl, small, 32 * s, ImVec2(x + 10 * s, y0 + cat.count * gap + 14 * s),
            Label(a), help, w);
}

void DashboardScene::DrawAdvanced(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float slide = 200 * s * (1 - m_page_anim);

    auto on_off = [](bool on) { return on ? "On" : "Off"; };
    const struct {
        const char *name, *value, *help;
    } rows[ADV__COUNT] = {
        { "Xbox memory",
          g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128 ? "128 MB" :
                                                               "64 MB",
          "64 MB like a retail Xbox: what every game is made for. (128 MB "
          "developer-kit mode is off on the PS5.)" },
        { "Video output", AvpackName(g_config.sys.avpack),
          "HDTV lets games use 480p (and 720p with a 720p patch on: XPSemu "
          "sets that for each game as it starts)." },
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

    float ty = y0 + visible * gap + 12 * s;
    const char *note = m_restart_needed ?
                           "* Changed: close and reopen XPSemu to apply." :
                           "* Takes effect the next time XPSemu starts.";
    float note_y = HintsTop() - TextSize(small, 30 * s, note).y - 6 * s;
    float end = TextFit(dl, small, 32 * s, ImVec2(x + 10 * s, ty), Label(a),
                        rows[m_adv_setting].help, w, note_y - 6 * s);
    Text(dl, small, 30 * s, ImVec2(x + 10 * s, std::max(end + 6 * s, note_y)),
         m_restart_needed ? Lime(a) : Label(a * 0.6f), note);
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
    snprintf(version, sizeof(version), "XPSemu Alpha 2, based on xemu %s",
             xemu_version);
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

    // Side by side: restart XPSemu (from its startup, as if opened again),
    // eject. (XPSemu is closed the PS5's way.)
    bool asking = m_time < m_sys_ask;
    const char *actions[] = { asking && m_system_row == 0 ? "Again: ends the game" :
                                                            "Restart XPSemu",
                              "Eject the disc" };
    float ay = y + 8 * 58 * s + 50 * s, w = 440 * s, h = 84 * s, gap = 470 * s;
    for (int i = 0; i < 2; i++) {
        float bx = x + i * gap;
        bool selected = i == m_system_row;
        Bar(dl, ImVec2(bx, ay), ImVec2(bx + w, ay + h), s, selected, a);
        ImVec2 ts = TextSize(font, 40 * s, actions[i]);
        Text(dl, font, 40 * s, ImVec2(bx + (w - ts.x) / 2, ay + 20 * s),
             selected ? Ink(a) : Label(a), actions[i]);
    }
    float fx = x + m_system_row * gap;
    Focus(dl, ImVec2(fx, ay), ImVec2(fx + w, ay + h), s, m_time, a);
}

void DashboardScene::DrawHints(float s)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float a = m_alpha;

    struct Hint { Glyph glyph; const char *text; };
    std::vector<Hint> hints;
    if (m_setup) {
        hints.push_back({ GLYPH_CROSS, m_setup_ok ? "Start XPSemu" : "Check now" });
    } else if (!m_in_page) {
        hints.push_back({ GLYPH_CROSS, m_np_focus ? "Resume" :
                                       m_shelf ? "Play" :
                                       m_page == MENU_XBOX_DASHBOARD ?
                                                 "Start" :
                                                 "Open" });
        if (m_shelf) {
            hints.push_back({ GLYPH_TRIANGLE, "Game settings" });
        }
        if (NowPlayingGame()) {
            hints.push_back({ GLYPH_SQUARE, m_time < m_eject_ask ?
                                                "Press again to eject" :
                                                "Eject disc" });
        }
        if (m_can_return) {
            hints.push_back({ GLYPH_CIRCLE, m_game_started ?
                                                "Back to the game" :
                                                "Back to the Xbox" });
        }
    } else {
        if (m_gs_open && m_kb_open) {
            hints.push_back({ GLYPH_CROSS, "Type" });
            hints.push_back({ GLYPH_SQUARE, "Delete" });
            hints.push_back({ GLYPH_TRIANGLE, "Space" });
        } else if (m_gs_open && m_gs_patches &&
                   m_patch_row >= (int)m_gs_patch_items.size()) {
            hints.push_back({ GLYPH_CROSS, "Get" });
        } else if (m_gs_open && m_gs_patches && m_patch_row < (int)m_gs_patch_items.size() &&
                   FromStore(*m_gs_patch_items[m_patch_row].file)) {
            hints.push_back({ GLYPH_CROSS, "Turn on/off" });
            hints.push_back({ GLYPH_SQUARE, "Remove" });
        } else if (m_gs_open) {
            hints.push_back({ GLYPH_CROSS,
                              m_gs_patches ? "Turn on/off" :
                              m_gs_row == GS_RESET ? "Reset" :
                              m_gs_row == GS_PATCHES ? "Open" :
                              m_gs_row == GS_NAME ? "Rename" :
                              m_gs_row == GS_SHORTCUT ?
                                  (m_gs_shortcut.empty() ? "Create" : "Remove") :
                                                       "Change" });
        } else if (m_page == PAGE_GAMES && !m_games.empty() && m_view) {
            hints.push_back({ GLYPH_LSTICK, "Turn" });
            hints.push_back({ GLYPH_RSTICK, "Closer" });
            hints.push_back({ GLYPH_CROSS, "Flip over" });
            hints.push_back({ GLYPH_TRIANGLE, "Straighten" });
        } else if (m_page == PAGE_GAMES && !m_games.empty()) {
            hints.push_back({ GLYPH_CROSS, "Play" });
            hints.push_back({ GLYPH_SQUARE, "View case" });
            hints.push_back({ GLYPH_TRIANGLE, "Game settings" });
        } else if (m_page == PAGE_SETTINGS && m_all_page) {
            hints.push_back({ GLYPH_CROSS, "Turn on/off" });
        } else if (m_page == PAGE_SETTINGS && m_pad_page) {
            hints.push_back({ GLYPH_CROSS, "Change" });
            hints.push_back({ GLYPH_TRIANGLE, "Standard layout" });
        } else if (m_page == PAGE_SETTINGS) {
            if (!m_advanced || !AdvancedLocked(m_adv_setting)) {
                hints.push_back({ GLYPH_CROSS,
                                  !m_advanced && m_settings_cat < 0 ?
                                      "Open" :
                                      "Change" });
            }
        } else if (m_page == PAGE_SYSTEM) {
            hints.push_back({ GLYPH_CROSS, "Select" });
        }
        hints.push_back({ GLYPH_CIRCLE, m_game_started && m_can_return ?
                                            "Back   (hold: back to the game)" :
                                            "Back" });
    }

    float x = 90 * s, y = size.y - 80 * s;
    for (const auto &h : hints) {
        DrawGlyph(dl, ImVec2(x + 22 * s, y), 22 * s, h.glyph, a);
        if (h.glyph == GLYPH_CIRCLE && m_back_hold > 0.05f) {
            // Quick resume filling up as Circle is held.
            float t = std::min(m_back_hold / 0.6f, 1.0f);
            dl->PathArcTo(ImVec2(x + 22 * s, y), 31 * s, -IM_PI / 2,
                          -IM_PI / 2 + 2 * IM_PI * t, 32);
            dl->PathStroke(Lime(a), 0, 4 * s);
        }
        Text(dl, small, 32 * s, ImVec2(x + 56 * s, y - 18 * s),
             White(a * 0.85f), h.text);
        x += 100 * s + TextSize(small, 32 * s, h.text).x;
    }

}

//
// Attract mode: two minutes without a press and the dashboard shows the
// games, one cover at a time, slowly; any press brings it back.
//

static const float kAttractIdle = 120, kAttractEach = 8, kAttractFade = 1.2f;

bool DashboardScene::UpdateAttract(float dt)
{
    uint32_t held = g_input_mgr.CombinedButtons();
    bool stick = ImGui::IsKeyDown(ImGuiKey_GamepadLStickLeft) ||
                 ImGui::IsKeyDown(ImGuiKey_GamepadLStickRight) ||
                 ImGui::IsKeyDown(ImGuiKey_GamepadLStickUp) ||
                 ImGui::IsKeyDown(ImGuiKey_GamepadLStickDown);
    bool woke = false;
    if (held || stick || !m_visible || m_setup) {
        if (m_attract) {
            m_attract = false;
            woke = true;
            m_prev_buttons = held; // The press only wakes it
            UiSoundPlay(UI_SOUND_OPEN);
        }
        m_idle = 0;
    } else {
        m_idle += dt;
    }

    // Covers load a few at a time while it waits, so they're ready.
    if (!m_attract && m_idle > kAttractIdle - 20) {
        for (const auto &g : m_games) {
            if (!Art(g.path).cover_tried) {
                GameCover(g);
                break;
            }
        }
    }
    if (!m_attract && m_idle > kAttractIdle) {
        m_attract_games.clear();
        for (int i = 0; i < (int)m_games.size(); i++) {
            if (Art(m_games[i].path).cover) {
                m_attract_games.push_back(i);
            }
        }
        for (int i = (int)m_attract_games.size() - 1; i > 0; i--) {
            std::swap(m_attract_games[i], m_attract_games[rand() % (i + 1)]);
        }
        if (!m_attract_games.empty()) {
            m_attract = true;
            m_attract_t = 0;
        } else {
            m_idle = 0; // No covers to show: wait again
        }
    }
    if (m_attract) {
        m_attract_t += dt;
    }
    m_attract_alpha = Approach(m_attract_alpha, m_attract ? 1 : 0, dt,
                               m_attract ? 1.5f : 6);
    return m_attract || woke;
}

void DashboardScene::DrawAttract(float s, float a)
{
    int n = m_attract_games.size();
    if (!n) {
        return;
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *big = g_font_mgr.m_menu_font;
    ImFont *small = g_font_mgr.m_menu_font_small;
    dl->AddRectFilled(ImVec2(0, 0), size, Rgba(0, 0, 0, a));

    int step = (int)(m_attract_t / kAttractEach);
    float local = fmodf(m_attract_t, kAttractEach);
    // The current game, and the next fading in over the last moment.
    for (int layer = 0; layer < 2; layer++) {
        float la = a;
        float lt = local;
        int index = step;
        if (layer == 1) {
            if (local < kAttractEach - kAttractFade) {
                break;
            }
            la = a * (local - (kAttractEach - kAttractFade)) / kAttractFade;
            lt = local - kAttractEach;
            index = step + 1;
        }
        const Game &game = m_games[m_attract_games[index % n]];
        XemuTexture cover = Art(game.path).cover;
        if (!cover) {
            continue;
        }
        // The cover blurred over the screen, darkened.
        DrawBlurredFill(dl, cover, false, size, 40 * s, m_attract_t, la);
        dl->AddRectFilledMultiColor(ImVec2(0, 0), size, Rgba(0, 0, 0, la * 0.85f),
                                    Rgba(0, 0, 0, la * 0.45f),
                                    Rgba(0, 0, 0, la * 0.55f),
                                    Rgba(0, 0, 0, la * 0.9f));

        // The cover itself, slowly coming closer, with its reflection.
        float k = (lt + kAttractFade) / (kAttractEach + kAttractFade);
        float h = 720 * s * (1 + 0.05f * k), w = h * 0.72f;
        ImVec2 c(1360 * s - 30 * s * k, 500 * s);
        ImVec2 p0(c.x - w / 2, c.y - h / 2), p1(c.x + w / 2, c.y + h / 2);
        dl->AddImageRounded((ImTextureID)cover, p0, p1, ImVec2(0, 0),
                            ImVec2(1, 1), White(la), 10 * s);
        dl->AddRect(p0, p1, Line(la * 0.8f), 10 * s, 0, 2 * s);
        float rh = h * 0.18f;
        int v0 = dl->VtxBuffer.Size;
        dl->AddImageQuad((ImTextureID)cover, ImVec2(p0.x, p1.y + 6 * s),
                         ImVec2(p1.x, p1.y + 6 * s),
                         ImVec2(p1.x, p1.y + 6 * s + rh),
                         ImVec2(p0.x, p1.y + 6 * s + rh), ImVec2(0, 1),
                         ImVec2(1, 1), ImVec2(1, 1 - 0.18f), ImVec2(0, 1 - 0.18f),
                         White(la * 0.3f));
        FadeDown(dl, v0, p1.y + 6 * s, p1.y + 6 * s + rh);

        // Its name and time played, on the left: the time under however
        // many lines the name takes.
        float tx = 150 * s + 20 * s * k, wrap = 820 * s;
        ImVec2 ns = big->CalcTextSizeA(76 * s, FLT_MAX, wrap, game.name.c_str());
        float ny = 640 * s - ns.y; // Long names grow upwards
        Text(dl, big, 76 * s, ImVec2(tx, ny), White(la), game.name.c_str(),
             wrap);
        GamePlay play = GamePlayGet(KeyOf(game));
        std::string played = play.seconds ?
            "Played " + GamePlayTimeText(play.seconds) : "Not played yet";
        Text(dl, small, 32 * s, ImVec2(tx, ny + ns.y + 18 * s), Lime(la * 0.9f),
             played.c_str());
    }

    DrawTitle(s, a, ImVec2(90 * s, 70 * s));
    float pulse = 0.55f + 0.45f * sinf(m_time * 2);
    const char *wake = "Press any button";
    ImVec2 ws = TextSize(small, 30 * s, wake);
    Text(dl, small, 30 * s, ImVec2(150 * s, size.y - 110 * s),
         White(a * pulse * 0.8f), wake);
    (void)ws;
}

// Top right: the controller, the date and the time.
extern "C" int xemu_ps5_pad_connected(void); // ui/xemu-input-ps5.c

static void DrawPadIcon(ImDrawList *dl, ImVec2 c, float s, ImU32 col, float a)
{
    float w = 22 * s, h = 9 * s;
    dl->AddRectFilled(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h * 0.6f),
                      col, 8 * s);
    dl->AddCircleFilled(ImVec2(c.x - w * 0.68f, c.y + h * 0.55f), 7.5f * s,
                        col, 16);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.68f, c.y + h * 0.55f), 7.5f * s,
                        col, 16);
    ImU32 ink = Rgba(4, 28, 4, a);
    dl->AddCircleFilled(ImVec2(c.x - w * 0.36f, c.y + 1 * s), 3 * s, ink, 10);
    dl->AddCircleFilled(ImVec2(c.x + w * 0.36f, c.y + 1 * s), 3 * s, ink, 10);
}

void DashboardScene::DrawStatus(float s)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float a = m_alpha;

    time_t now = time(NULL);
    struct tm lt;
    if (!localtime_r(&now, &lt)) {
        return;
    }
    static const char *const days[] = { "Sun", "Mon", "Tue", "Wed",
                                        "Thu", "Fri", "Sat" };
    static const char *const months[] = { "Jan", "Feb", "Mar", "Apr",
                                          "May", "Jun", "Jul", "Aug",
                                          "Sep", "Oct", "Nov", "Dec" };
    char clock[16], date[32];
    snprintf(clock, sizeof(clock), "%02d:%02d", lt.tm_hour, lt.tm_min);
    snprintf(date, sizeof(date), "%s %d %s", days[lt.tm_wday % 7],
             lt.tm_mday, months[lt.tm_mon % 12]);

    float right = size.x - 90 * s, y = 26 * s;
    (void)font;
    ImVec2 cs = TextSize(small, 28 * s, clock);
    Text(dl, small, 28 * s, ImVec2(right - cs.x, y), Label(a * 0.85f), clock);
    ImVec2 ds = TextSize(small, 28 * s, date);
    float dx = right - cs.x - 18 * s - ds.x;
    Text(dl, small, 28 * s, ImVec2(dx, y), Label(a * 0.85f), date);

    float sx = dx - 22 * s;
    dl->AddLine(ImVec2(sx, y + 9 * s), ImVec2(sx, y + cs.y - 7 * s),
                Line(a * 0.6f), 1.5f * s);
    bool pad = xemu_ps5_pad_connected();
    DrawPadIcon(dl, ImVec2(sx - 42 * s, y + cs.y / 2), s,
                pad ? Lime(a * 0.9f) : Rgba(255, 120, 90, a), a);
}

//
// First run: the Xbox files XPSemu needs, checked before the dashboard.
// Missing ones are listed with where they go; the Xbox can only use them
// after XPSemu starts again (it's put together at start).
//

struct SetupItem {
    std::string name, what, path, status;
    bool ok, required;
};
static std::vector<SetupItem> g_setup_items;

static bool SetupRecheck(int games)
{
    auto size_of = [](const char *p) -> long long {
        std::error_code ec;
        if (!p || !p[0]) {
            return -1;
        }
        auto n = std::filesystem::file_size(p, ec);
        return ec ? -1 : (long long)n;
    };
    g_setup_items.clear();
    auto add = [&](const char *name, const char *what, const char *path,
                   long long want) {
        SetupItem it{ name, what, path && path[0] ? path : "(not set)", "",
                      false, true };
        long long n = size_of(path);
        if (n < 0) {
            it.status = "Missing";
        } else if (want > 0 && n != want) {
            it.status = "Wrong size: " + std::to_string(n) + " bytes, needs " +
                        std::to_string(want);
        } else {
            it.status = "Found";
            it.ok = true;
        }
        g_setup_items.push_back(it);
    };
    add("MCPX boot ROM", "The Xbox's 512-byte boot ROM",
        g_config.sys.files.bootrom_path, 512);
    add("Flash BIOS", "The Xbox BIOS, e.g. Complex 4627",
        g_config.sys.files.flashrom_path, 0);
    add("Hard disk image", "A prepared Xbox hard disk",
        g_config.sys.files.hdd_path, 0);
#ifdef __PROSPERO__
    extern int xemu_ps5_hdd_created;
    if (xemu_ps5_hdd_created && g_setup_items.back().ok) {
        g_setup_items.back().status = "Made by XPSemu";
    }
#endif
    g_setup_items.push_back(
        { "Games", "Your .iso games (you can add them later)",
          std::string(g_config.general.games_dir[0] ?
                          g_config.general.games_dir :
                          "/data/xemu/games"),
          games ? std::to_string(games) + " found" : "None yet", games > 0,
          false });

    bool missing = false;
    for (const auto &it : g_setup_items) {
        missing |= it.required && !it.ok;
    }
    return missing;
}

bool DashboardSetupNeeded()
{
    return SetupRecheck(0);
}

void DashboardScene::DrawSetup(float s, float a)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 size = ImGui::GetIO().DisplaySize;
    ImFont *big = g_font_mgr.m_menu_font;
    ImFont *font = g_font_mgr.m_menu_font_medium;
    ImFont *small = g_font_mgr.m_menu_font_small;
    float cx = size.x / 2;
    ImU32 red = Rgba(255, 95, 80, a);

    // Looked for again every 2 s, so a file copied over FTP just appears.
    if (!m_setup_ok && m_time >= m_setup_next) {
        m_setup_next = m_time + 2;
        if (!SetupRecheck((int)m_games.size())) {
            ScanGames();
            SetupRecheck((int)m_games.size());
            m_setup_ok = true;
            UiSoundPlay(UI_SOUND_LAUNCH);
        }
    }

    // The emblem.
    ImVec2 ec(cx, 190 * s);
    if (g_boot_logo) {
        float r = 92 * s;
        dl->AddImage((ImTextureID)g_boot_logo, ImVec2(ec.x - r, ec.y - r),
                     ImVec2(ec.x + r, ec.y + r), ImVec2(0, 0), ImVec2(1, 1),
                     White(a));
    }
    const char *title = m_setup_ok ? "You're all set!" : "Welcome to XPSemu";
    ImVec2 ts = TextSize(big, 64 * s, title);
    Text(dl, big, 64 * s, ImVec2(cx - ts.x / 2, 310 * s), White(a), title);
    const char *sub = m_setup_ok ?
        "Every file is in place. Press Cross and XPSemu starts with them." :
        "XPSemu needs the MCPX boot ROM and the BIOS from your own Xbox (the "
        "hard disk is made for you). Copy them to /data/xemu on the PS5 (FTP): "
        "each turns green when it's there.";
    float sw = 1300 * s;
    ImVec2 ss = small->CalcTextSizeA(30 * s, FLT_MAX, sw, sub);
    Text(dl, small, 30 * s, ImVec2(cx - std::min(ss.x, sw) / 2, 395 * s),
         Label(a), sub, sw);

    // The three files, side by side.
    int required = 0, have = 0;
    for (const auto &it : g_setup_items) {
        required += it.required;
        have += it.required && it.ok;
    }
    float tw = 440 * s, th = 300 * s, gap = 40 * s;
    float x0 = cx - (3 * tw + 2 * gap) / 2, y0 = 500 * s;
    int k = 0;
    for (const auto &it : g_setup_items) {
        if (!it.required || k >= 3) {
            continue;
        }
        ImVec2 p0(x0 + k * (tw + gap), y0), p1(p0.x + tw, p0.y + th);
        k++;
        ImU32 edge = it.ok ? Lime(a) : red;
        dl->AddRectFilled(p0, p1, Rgba(0, 18, 0, a * 0.55f), 22 * s);
        dl->AddRectFilledMultiColor(p0, ImVec2(p1.x, p0.y + th * 0.4f),
                                    it.ok ? Rgba(120, 230, 40, a * 0.10f) :
                                            Rgba(255, 95, 80, a * 0.08f),
                                    it.ok ? Rgba(120, 230, 40, a * 0.10f) :
                                            Rgba(255, 95, 80, a * 0.08f),
                                    Rgba(0, 0, 0, 0), Rgba(0, 0, 0, 0));
        dl->AddRect(p0, p1, edge, 22 * s, 0, 3 * s);
        // A green tick or a red cross.
        ImVec2 c(p0.x + tw / 2, p0.y + 72 * s);
        if (it.ok) {
            dl->AddCircleFilled(c, 40 * s, Lime(a), 40);
            dl->AddLine(ImVec2(c.x - 18 * s, c.y + 1 * s),
                        ImVec2(c.x - 5 * s, c.y + 14 * s), Ink(a), 6 * s);
            dl->AddLine(ImVec2(c.x - 5 * s, c.y + 14 * s),
                        ImVec2(c.x + 19 * s, c.y - 14 * s), Ink(a), 6 * s);
        } else {
            float pulse = 0.75f + 0.25f * sinf(m_time * 4);
            dl->AddCircleFilled(c, 40 * s, Rgba(255, 95, 80, a * pulse), 40);
            dl->AddLine(ImVec2(c.x - 14 * s, c.y - 14 * s),
                        ImVec2(c.x + 14 * s, c.y + 14 * s), White(a), 6 * s);
            dl->AddLine(ImVec2(c.x + 14 * s, c.y - 14 * s),
                        ImVec2(c.x - 14 * s, c.y + 14 * s), White(a), 6 * s);
        }
        ImVec2 ns = TextSize(font, 40 * s, it.name.c_str());
        Text(dl, font, 40 * s, ImVec2(c.x - ns.x / 2, p0.y + 128 * s), White(a),
             it.name.c_str());
        std::string file = it.path;
        size_t slash = file.rfind('/');
        if (slash != std::string::npos) {
            file = file.substr(slash + 1);
        }
        ImVec2 fs = TextSize(small, 28 * s, file.c_str());
        Text(dl, small, 28 * s, ImVec2(c.x - fs.x / 2, p0.y + 182 * s),
             Label(a * 0.85f), file.c_str());
        std::string status = it.ok ? "Found" : it.status;
        ImVec2 st = small->CalcTextSizeA(28 * s, FLT_MAX, tw - 40 * s, status.c_str());
        Text(dl, small, 28 * s,
             ImVec2(c.x - std::min(st.x, tw - 40 * s) / 2, p0.y + 228 * s),
             it.ok ? Lime(a) : red, status.c_str(), tw - 40 * s);
    }

    // Under them: the games, and how far along.
    float y = y0 + th + 46 * s;
    for (const auto &it : g_setup_items) {
        if (it.required) {
            continue;
        }
        std::string line = "Games:  " + it.status + "  in  " + it.path +
                           (it.ok ? "" : "  (add them any time)");
        ImVec2 ls = TextSize(small, 30 * s, line.c_str());
        Text(dl, small, 30 * s, ImVec2(cx - ls.x / 2, y),
             it.ok ? Lime(a) : Label(a * 0.8f), line.c_str());
        y += 52 * s;
    }
    char ready[64];
    snprintf(ready, sizeof(ready), "%d of %d ready", have, required);
    float bw = 520 * s, bh = 12 * s;
    ImVec2 b0(cx - bw / 2, y + 8 * s);
    dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + bh), Rgba(0, 0, 0, a * 0.4f),
                      bh / 2);
    if (have > 0) {
        dl->AddRectFilled(b0, ImVec2(b0.x + bw * have / std::max(required, 1),
                                     b0.y + bh),
                          Lime(a), bh / 2);
    }
    ImVec2 rs = TextSize(small, 28 * s, ready);
    Text(dl, small, 28 * s, ImVec2(cx - rs.x / 2, b0.y + 26 * s),
         Label(a * 0.9f), ready);
    if (!m_setup_ok) {
        const char *watching = "Looking for them every few seconds...";
        ImVec2 ws = TextSize(small, 26 * s, watching);
        Text(dl, small, 26 * s, ImVec2(cx - ws.x / 2, b0.y + 66 * s),
             Label(a * (0.45f + 0.25f * sinf(m_time * 3))), watching);
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
    g_xemu_ps5_pad_hold = g_dashboard.Visible();
    float dt = ImGui::GetIO().DeltaTime;

    // Back in the game: its sound comes up over half a second.
    static bool was_visible;
    static float ramp = 1;
    bool visible = g_dashboard.Visible();
    if (was_visible && !visible && !g_curtain.active) {
        ramp = 0;
    }
    was_visible = visible;
    if (ramp < 1 && !g_curtain.active) {
        ramp = std::min(ramp + dt / 0.5f, 1.0f);
        g_xemu_apu_ui_gain = ramp * ramp;
    }

    GameLogTick();
    g_dashboard.WatchTick();
    GamePlayTick(ImGui::GetIO().DeltaTime,
                 runstate_is_running() && !g_scene_mgr.IsDisplayingScene());
}
