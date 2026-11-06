// XOG: the dashboard's process. It draws, plays its sounds and writes settings; a game's start is a
// file and a restart of the title, which then runs the game with none of this loaded into its work.
// GPL-3.0-or-later.
#include <pthread.h>
#include "xog_title.h"
#include <stdarg.h>
#include <time.h>

#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>

#include <imgui_impl_vulkan.h>

#include "common.hh"
#include "dashboard.hh"
#include "font-manager.hh"
#include "input-manager.hh"
#include "scene-manager.hh"
#include "ui-sounds.hh"
#include "viewport-manager.hh"

extern "C" {
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, unsigned int len, unsigned int freq, unsigned int param);
int sceAudioOutOutput(int handle, const void *buf);
int sceSystemServiceLoadExec(const char *path, char *const argv[]);
void xemu_ps5_ui_sound_mix(int16_t *buf, int frames);
void nc_log(const char *tag, const char *fmt, ...);
void rx_log_drain(void);
}
void DashboardTick();
extern DashboardScene g_dashboard;

XogConfig g_cfg;

extern "C" {
float g_xemu_apu_ui_gain = 1;
bool g_xemu_ui_hold_frame;
int g_xemu_ps5_pad_hold;
int xemu_ps5_import_ok(const char *) { return 1; }   // report_imports logs any that's missing
int xemu_ps5_pad_connected(void) { return g_input_mgr.Connected(); }
}

Scene::~Scene() {}
void Scene::Show() {}
void Scene::Hide() {}
bool Scene::IsAnimating() { return false; }
bool Scene::Draw() { return false; }

static const char *const kBase = "/data/xpsemu/";

void xog_ui_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    nc_log("xog-ui", "%s", line);
}

const char *xemu_settings_get_base_path(void)
{
    return kBase;
}

static std::string SettingsFile()
{
    return std::string(kBase) + "settings.txt";
}

void xemu_settings_save(void)
{
    std::ofstream f(SettingsFile());
    const XogConfig &c = g_cfg;
    f << "scale=" << c.scale << "\naspect=" << c.aspect << "\nfit=" << c.fit << "\nupscale=" << c.upscale << "\naa=" << c.aa << "\nfps_cap=" << c.fps_cap
      << "\nvolume=" << c.volume << "\nfps=" << c.fps << "\navpack=" << c.avpack << "\nlanguage=" << c.language
      << "\npal60=" << c.pal60 << "\nnotifications=" << c.notifications << "\nwidescreen_patches=" << c.autopatch_widescreen
      << "\nfps_patches=" << c.autopatch_60fps << "\nsounds=" << c.sounds << "\nmusic=" << c.music << "\ncovers=" << c.covers
      << "\nscale_chosen=1\nboot_logo=" << c.boot_logo << "\nsides=" << c.sides << "\nsides_style=" << c.sides_style << "\n";
}

void xemu_settings_load(void)
{
    std::ifstream f(SettingsFile());
    std::string line;
    XogConfig &c = g_cfg;
    bool scale_chosen = false;   // settings from before x2 was the default take it, once
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string k = line.substr(0, eq);
        const int v = atoi(line.c_str() + eq + 1);
        if (k == "scale") c.scale = std::clamp(v, 1, 6);
        else if (k == "scale_chosen") scale_chosen = true;
        else if (k == "aspect") c.aspect = v;
        else if (k == "fit") c.fit = v;
        else if (k == "upscale") c.upscale = v;
        else if (k == "aa") c.aa = std::clamp(v, 0, 3);
        else if (k == "fps_cap") c.fps_cap = std::clamp(v, 0, 1000);
        else if (k == "volume") c.volume = std::clamp(v, 0, 10);
        else if (k == "fps") c.fps = v;
        else if (k == "avpack") c.avpack = v;
        else if (k == "language") c.language = v;
        else if (k == "pal60") c.pal60 = v;
        else if (k == "notifications") c.notifications = v;
        else if (k == "widescreen_patches") c.autopatch_widescreen = v;
        else if (k == "fps_patches") c.autopatch_60fps = v;
        else if (k == "sounds") c.sounds = v;
        else if (k == "music") c.music = v;
        else if (k == "covers") c.covers = v;
        else if (k == "boot_logo") c.boot_logo = v;
        else if (k == "sides") c.sides = v;
        else if (k == "sides_style") c.sides_style = v;
    }
    if (!scale_chosen) {
        c.scale = 4;
    }
}

//
// Notifications: a line at the top of the screen for a few seconds.
//
struct Note {
    std::string text;
    double until;
};
static std::mutex g_notes_lock;
static std::deque<std::string> g_notes;
static Note g_note;

void xemu_queue_notification(const char *text)
{
    xog_ui_log("note: %s", text);
    if (!g_cfg.notifications) {
        return;
    }
    std::lock_guard<std::mutex> guard(g_notes_lock);
    g_notes.push_back(text);
}

static void DrawNotes(double now)
{
    if (g_note.text.empty() || now > g_note.until) {
        std::lock_guard<std::mutex> guard(g_notes_lock);
        g_note.text.clear();
        if (!g_notes.empty()) {
            g_note.text = g_notes.front();
            g_notes.pop_front();
            g_note.until = now + 4;
        }
    }
    if (g_note.text.empty()) {
        return;
    }
    ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float s = io.DisplaySize.y / 1080.0f, size = 30 * s;
    ImFont *font = g_font_mgr.m_menu_font_medium;
    const ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, g_note.text.c_str());
    const ImVec2 p((io.DisplaySize.x - ts.x) / 2, 36 * s);
    const float a = std::clamp((float)(g_note.until - now) * 2, 0.0f, 1.0f);
    dl->AddRectFilled(ImVec2(p.x - 24 * s, p.y - 12 * s), ImVec2(p.x + ts.x + 24 * s, p.y + ts.y + 12 * s),
                      IM_COL32(12, 45, 6, (int)(225 * a)), 8 * s);
    dl->AddText(font, size, p, IM_COL32(205, 245, 45, (int)(255 * a)), g_note.text.c_str());
}

//
// Restarts
//
static void Restart()
{
    rx_log_drain();
    for (const char *path : { "/app0/eboot.bin", "/data/homebrew/" XOG_TITLE "/eboot.bin" }) {
        const int rc = sceSystemServiceLoadExec(path, NULL);
        xog_ui_log("restart via %s refused: %#x", path, rc);
        rx_log_drain();
    }
}

// A game writes logs, its cache and saves. With no room it falls back here with nothing to show for it.
static bool StorageHasRoom()
{
    const std::string test = std::string(kBase) + "space-check.tmp";
    FILE *f = fopen(test.c_str(), "wb");
    if (!f) {
        return false;
    }
    static char block[1 << 16];
    bool ok = true;
    for (int i = 0; ok && i < 64; i++) {
        ok = fwrite(block, 1, sizeof(block), f) == sizeof(block);
    }
    ok = fclose(f) == 0 && ok;
    unlink(test.c_str());
    return ok;
}

void xog_start_game(const std::string &lines)
{
    if (!StorageHasRoom()) {
        xog_ui_log("storage full: start refused");
        xemu_queue_notification("PS5 system storage is full: free up some space, then start the game again");
        return;
    }
    const std::string tmp = std::string(kBase) + "launch.tmp", file = std::string(kBase) + "launch.txt";
    {
        std::ofstream f(tmp);
        f << lines;
    }
    rename(tmp.c_str(), file.c_str());
    xog_ui_log("starting:\n%s", lines.c_str());
    Restart();
    xemu_queue_notification("Couldn't start the game: close XPSemu and open it again");
    unlink(file.c_str());
}

void xog_restart_dashboard(void)
{
    Restart();
}

//
// Sound
//
static void *AudioThread(void *)
{
    sceAudioOutInit();
    const int handle = sceAudioOutOpen(255, 0, 0, 256, 48000, 1);
    xog_ui_log("sound: port %d", handle);
    if (handle < 0) {
        return NULL;
    }
    static int16_t block[256 * 2] __attribute__((aligned(64)));
    for (;;) {
        memset(block, 0, sizeof(block));
        xemu_ps5_ui_sound_mix(block, 256);
        sceAudioOutOutput(handle, block);
    }
    return NULL;
}

//
// The loop
//
void xog_ui_main(void)
{
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    auto since = [&]() {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        return (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    };
    xemu_settings_load();
    xemu_ps5_map_load();
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    if (!xog_vk_init()) {
        xog_ui_log("no display: the dashboard can't run");
        return;
    }
    xog_ui_log("start: Vulkan up at %.0f ms", since());
    io.DisplaySize = ImVec2(3840, 2160);
    g_viewport_mgr.m_scale = 2;
    g_viewport_mgr.m_pixel_density = 1;
    g_font_mgr.Rebuild();
    xog_ui_log("start: fonts at %.0f ms", since());

    pthread_t t;
    pthread_create(&t, NULL, AudioThread, NULL);
    pthread_detach(t);

    g_dashboard.m_auto_shown = true;
    g_dashboard.m_startup_show = true;
    g_scene_mgr.PushScene(g_dashboard);
    struct timespec last;
    clock_gettime(CLOCK_MONOTONIC, &last);
    double now = 0;
    for (;;) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const double dt = (ts.tv_sec - last.tv_sec) + (ts.tv_nsec - last.tv_nsec) / 1e9;
        last = ts;
        now += dt;
        io.DeltaTime = dt > 0 ? (float)dt : 1.0f / 60;
        g_input_mgr.Update();
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        static int frames_seen;
        if (frames_seen < 3) {
            xog_ui_log("start: frame %d begins at %.0f ms", frames_seen, since());
        }
        g_scene_mgr.Draw();
        DashboardTick();
        DrawNotes(now);
        ImGui::Render();
        if (frames_seen < 3) {
            xog_ui_log("start: frame %d drawn at %.0f ms", frames_seen++, since());
        }
        xog_vk_present(ImGui::GetDrawData());
        static bool told;
        if (!told) {
            told = true;
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            xog_ui_log("first frame %.0f ms after the page's start",
                       (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
        }
    }
}

extern "C" void xog_ui_entry(void)
{
    xog_ui_main();
}
