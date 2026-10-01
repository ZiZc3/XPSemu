//
// XPSemu: a log for each game played
//
// Everything xemu prints goes to xemu.log (stderr, see xemu_ps5_early_init),
// from the start of XPSemu. This log is just one game: what it is and how
// it's set up, how it runs (a stats line every few seconds, and a warning
// when it stops drawing), and what the emulator printed meanwhile, copied
// from xemu.log as it grows. The crash handler copies the last of it.
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
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "common.hh"
#include "game-log.hh"
#include "scene-manager.hh"
#include "../xemu-settings.h"
#include "../xemu-os-utils.h"
#include "../../hw/xbox/xemu-timing.h"
#include "xemu-version.h"

#ifdef __PROSPERO__
extern "C" int sceKernelAvailableFlexibleMemorySize(size_t *available);
#endif

static int g_fd = -1;     // The game's log
static off_t g_from;      // How much of xemu.log (stderr) is copied
static double g_started;  // ImGui time the game started
static double g_next_copy, g_next_stats;
static double g_last_cpu, g_last_wall;
static unsigned int g_last_frames;
static int g_idle_checks; // Stats lines in a row without a frame
static bool g_warned_idle;
static double g_last_thread[XEMU_PS5_THREAD__ALL];
static unsigned int g_last_spv_hits, g_last_spv_misses;
static double g_next_cache_save;
static double g_next_profile;
static uint64_t g_last_ns[XT__COUNT];
static uint32_t g_last_count[XT__COUNT];

static void Write(const std::string &text);

static void WriteProfile(const char *when)
{
    char *report = xemu_ps5_profile_report();
    if (report) {
        Write(std::string("\n") + when + " " + report + "\n");
        free(report);
    }
}

// Files in a folder and its folders, and their size, for the cache lines.
static void CountFiles(const std::string &dir, long *files, long long *bytes)
{
    DIR *d = opendir(dir.c_str());
    if (!d) {
        return;
    }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') {
            continue;
        }
        std::string p = dir + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            CountFiles(p, files, bytes);
        } else {
            (*files)++;
            *bytes += st.st_size;
        }
    }
    closedir(d);
}

// How full the shader caches are: they should grow as games are played,
// and a cache that stays empty isn't working.
static std::string CacheSummary()
{
    std::string base = std::string(xemu_settings_get_base_path()) + "cache";
    long mesa_files = 0, spv_files = 0;
    long long mesa_bytes = 0, spv_bytes = 0;
    CountFiles(base + "/mesa", &mesa_files, &mesa_bytes);
    CountFiles(base + "/spirv", &spv_files, &spv_bytes);
    struct stat st;
    long long pipes = stat((base + "/vk-pipelines.bin").c_str(), &st) == 0 ?
                          (long long)st.st_size :
                          0;
    char buf[256];
    snprintf(buf, sizeof(buf),
             "SPIR-V %ld files (%lld KB), pipelines %lld KB, GPU code (RADV) "
             "%ld files (%lld KB)%s",
             spv_files, spv_bytes >> 10, pipes >> 10, mesa_files,
             mesa_bytes >> 10,
             g_config.perf.cache_shaders ? "" : " - shader cache is OFF");
    return buf;
}

static void Write(const std::string &text)
{
    if (g_fd >= 0) {
        (void)!write(g_fd, text.data(), text.size());
    }
}

extern "C" void xemu_ps5_game_log_copy(void)
{
    struct stat st;
    if (g_fd < 0 || fstat(STDERR_FILENO, &st) != 0) {
        return;
    }
    char buf[4096];
    while (g_from < st.st_size) {
        size_t want = st.st_size - g_from;
        ssize_t n = pread(STDERR_FILENO, buf,
                          want < sizeof(buf) ? want : sizeof(buf), g_from);
        if (n <= 0) {
            break;
        }
        (void)!write(g_fd, buf, n);
        g_from += n;
    }
}

static std::string FileName(const char *path)
{
    if (!path || !path[0]) {
        return "(none)";
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static std::string Elapsed(double seconds)
{
    char buf[32];
    int t = (int)seconds;
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    return buf;
}

void GameLogStart(const GameLogInfo &info)
{
    GameLogStop("another game started");

    std::string path = std::string(xemu_settings_get_base_path()) +
                       "xemu-game.log";
    rename(path.c_str(), (path + ".old").c_str());
    g_fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_fd < 0) {
        fprintf(stderr, "XPSemu: can't write %s\n", path.c_str());
        return;
    }
    struct stat st;
    g_from = fstat(STDERR_FILENO, &st) == 0 ? st.st_size : 0;

    char now[64];
    time_t t = time(NULL);
    strftime(now, sizeof(now), "%Y-%m-%d %H:%M:%S", localtime(&t));
    struct stat iso;
    long long size_mb = stat(info.path.c_str(), &iso) == 0 ?
                            (long long)(iso.st_size >> 20) : -1;
    char buf[1024];
    std::string h = "== XPSemu game log ==\n";
    snprintf(buf, sizeof(buf),
             "Started      %s\n"
             "XPSemu       based on xemu %s\n"
             "Game         %s\n"
             "File         %s (%lld MB, %s)\n"
             "Title ID     %08X   key %s\n"
             "XBE title    %s\n",
             now, xemu_version, info.name.c_str(), info.path.c_str(), size_mb,
             info.full_disc ? "full disc image - xemu needs XISO" : "XISO",
             info.title_id, info.key.c_str(),
             info.xbe_title.empty() ? "(none)" : info.xbe_title.c_str());
    h += buf;
    snprintf(buf, sizeof(buf),
             "Own settings %s\n"
             "Settings     resolution %dx, screen shape %d, fit %d, "
             "smoothing %s, volume %d%%\n"
             "             DSP %s, fast FPU %s, shader cache %s, "
             "skip boot animation %s\n"
             "Xbox         %s MB, AV pack %d\n"
             "Files        MCPX %s, BIOS %s, HDD %s, EEPROM %s\n"
             "GPU          %s\n"
             "Pinning      %s\n"
             "Caches       %s\n",
             info.overrides.empty() ? "none" : info.overrides.c_str(),
             g_config.display.quality.surface_scale,
             g_config.display.ui.aspect_ratio, g_config.display.ui.fit,
             g_config.display.filtering == CONFIG_DISPLAY_FILTERING_LINEAR ?
                 "on" :
                 "off",
             (int)(g_config.audio.volume_limit * 100),
             g_config.audio.use_dsp ? "on" : "off",
             g_config.perf.hard_fpu ? "on" : "off",
             g_config.perf.cache_shaders ? "on" : "off",
             g_config.general.skip_boot_anim ? "on" : "off",
             g_config.sys.mem_limit == CONFIG_SYS_MEM_LIMIT_128 ? "128" : "64",
             g_config.sys.avpack,
             FileName(g_config.sys.files.bootrom_path).c_str(),
             FileName(g_config.sys.files.flashrom_path).c_str(),
             FileName(g_config.sys.files.hdd_path).c_str(),
             FileName(g_config.sys.files.eeprom_path).c_str(),
             pgraph_vk_get_device_name(), xemu_ps5_pinning_summary(),
             CacheSummary().c_str());
    h += buf;
    h += "Patches      ";
    h += info.patches.empty() ? "none\n" : "\n" + info.patches;
    h += "\nEvery 5 s, two lines:\n"
         "  time, frames the game drew (FPS), last frame time (the slowest, "
         "and frames over 50 ms), whole CPU (% of one core), the Xbox's CPU "
         "and GPU threads and the UI (% of their core), free memory\n"
         "  draws, shaders compiled (+ found in the SPIR-V cache), pipelines "
         "built, surfaces down/up loaded, textures uploaded, waits for the "
         "PS5 GPU\n"
         "  and a third: milliseconds per second (and times) the emulator "
         "spent on each step: the GPU thread submitting work to the PS5 GPU "
         "and waiting for it, waiting for one-off jobs, copying surfaces "
         "back, and idle; the CPU thread running the game and idle; the UI "
         "getting the game's frame and presenting\n"
         "Other lines are the emulator's messages meanwhile (from "
         "xemu.log).\n\n";
    Write(h);

    g_started = g_next_copy = g_next_stats = ImGui::GetTime();
    g_next_stats += 5;
    g_last_wall = -1;
    g_last_frames = g_nv2a_stats.frame_count;
    g_idle_checks = 0;
    g_warned_idle = false;
    for (int i = 0; i < XEMU_PS5_THREAD__ALL; i++) {
        g_last_thread[i] = -1;
        xemu_ps5_thread_cpu_seconds(i, &g_last_thread[i]);
    }
    g_last_spv_hits = pgraph_vk_spirv_cache_hits;
    g_last_spv_misses = pgraph_vk_spirv_cache_misses;
    g_next_cache_save = g_started + 120;
    g_next_profile = g_started + 30;
    for (int i = 0; i < XT__COUNT; i++) {
        g_last_ns[i] = __atomic_load_n(&xemu_timing_ns[i], __ATOMIC_RELAXED);
        g_last_count[i] =
            __atomic_load_n(&xemu_timing_count[i], __ATOMIC_RELAXED);
    }
    xemu_ps5_profile_start();
    fprintf(stderr, "XPSemu: game log started: %s\n", info.name.c_str());
}

void GameLogStop(const char *why)
{
    if (g_fd < 0) {
        return;
    }
    pgraph_vk_request_cache_save(); // Keep what this game built
    xemu_ps5_game_log_copy();
    WriteProfile("Whole game:");
    xemu_ps5_profile_stop();
    Write("\nCaches now: " + CacheSummary() + "\n");
    Write("\n== Stopped (" + std::string(why) + ") after " +
          Elapsed(ImGui::GetTime() - g_started) + " ==\n");
    close(g_fd);
    g_fd = -1;
}

void GameLogNote(const std::string &text)
{
    fprintf(stderr, "XPSemu: %s\n", text.c_str());
    if (g_fd >= 0) {
        xemu_ps5_game_log_copy();
        Write("[" + Elapsed(ImGui::GetTime() - g_started) + "] >> " + text +
              "\n");
    }
}

void GameLogTick()
{
    if (g_fd < 0) {
        return;
    }
    double now = ImGui::GetTime();
    if (now >= g_next_copy) {
        xemu_ps5_game_log_copy();
        g_next_copy = now + 1;
    }
    if (now >= g_next_profile) { // Also for when the app is just closed
        xemu_ps5_game_log_copy();
        WriteProfile("So far:");
        g_next_profile = now + 120;
    }
    if (now >= g_next_cache_save) { // Closing the app gives no chance later
        pgraph_vk_request_cache_save();
        g_next_cache_save = now + 120;
    }
    if (now < g_next_stats) {
        return;
    }
    double interval = now - g_next_stats + 5;
    g_next_stats = now + 5;

    struct timespec ts;
    double cpu = 0;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
        cpu = ts.tv_sec + ts.tv_nsec / 1e9;
    }
    int cpu_percent = g_last_wall >= 0 ?
                          (int)((cpu - g_last_cpu) / (now - g_last_wall) * 100) :
                          -1;
    g_last_cpu = cpu;
    g_last_wall = now;

    unsigned int frames = g_nv2a_stats.frame_count - g_last_frames;
    g_last_frames = g_nv2a_stats.frame_count;
    int mspf = g_nv2a_stats.frame_history[(g_nv2a_stats.frame_ptr +
                                           NV2A_PROF_NUM_FRAMES - 1) %
                                          NV2A_PROF_NUM_FRAMES].mspf;
    if (mspf < 0 || mspf > 60000) {
        mspf = 0;
    }
    long free_mb = -1;
#ifdef __PROSPERO__
    size_t available = 0;
    if (sceKernelAvailableFlexibleMemorySize(&available) == 0) {
        free_mb = (long)(available >> 20);
    }
#endif
    bool running = runstate_is_running();
    bool in_menu = g_scene_mgr.IsDisplayingScene();

    // The frames since the last line: slowest, those over 50 ms, and what
    // the GPU emulation did (nv2a's profile counters, per frame).
    unsigned int n = std::min(frames, (unsigned int)NV2A_PROF_NUM_FRAMES);
    int worst = 0, slow = 0;
    long c[NV2A_PROF__COUNT] = { 0 };
    for (unsigned int j = 0; j < n; j++) {
        const auto &f = g_nv2a_stats.frame_history
            [(g_nv2a_stats.frame_ptr + NV2A_PROF_NUM_FRAMES - 1 - j) %
             NV2A_PROF_NUM_FRAMES];
        if (f.mspf < 0 || f.mspf > 60000) {
            continue; // Before the first frame (no previous one)
        }
        worst = std::max(worst, f.mspf);
        slow += f.mspf > 50;
        for (int k = 0; k < NV2A_PROF__COUNT; k++) {
            c[k] += f.counters[k];
        }
    }
    long waits = 0;
    for (int k = NV2A_PROF_FINISH_VERTEX_BUFFER_DIRTY;
         k <= NV2A_PROF_FINISH_STALLED; k++) {
        waits += c[k];
    }
    int thread[XEMU_PS5_THREAD__ALL];
    for (int i = 0; i < XEMU_PS5_THREAD__ALL; i++) {
        double t;
        thread[i] = -1;
        if (xemu_ps5_thread_cpu_seconds(i, &t)) {
            if (g_last_thread[i] >= 0) {
                thread[i] = (int)((t - g_last_thread[i]) / interval * 100);
            }
            g_last_thread[i] = t;
        }
    }
    unsigned int hits = pgraph_vk_spirv_cache_hits - g_last_spv_hits;
    unsigned int misses = pgraph_vk_spirv_cache_misses - g_last_spv_misses;
    g_last_spv_hits = pgraph_vk_spirv_cache_hits;
    g_last_spv_misses = pgraph_vk_spirv_cache_misses;

    char buf[512];
    snprintf(buf, sizeof(buf),
             "[%s] %5.1f FPS  %4d ms (worst %d, %d slow)  CPU %3d%%  "
             "Xbox CPU %3d%%  Xbox GPU %3d%%  UI %3d%%  free %4ld MB%s\n"
             "           draws %ld  shaders +%u (%u from cache)  "
             "pipelines +%ld  surfaces down %ld up %ld  textures %ld  "
             "GPU waits %ld\n",
             Elapsed(now - g_started).c_str(), frames / interval, mspf, worst,
             slow, cpu_percent, thread[XEMU_PS5_THREAD_CPU],
             thread[XEMU_PS5_THREAD_GPU], thread[XEMU_PS5_THREAD_UI],
             free_mb,
             !running ? "  (paused)" : in_menu ? "  (in the dashboard)" : "",
             c[NV2A_PROF_BEGIN_ENDS], misses, hits, c[NV2A_PROF_PIPELINE_GEN],
             c[NV2A_PROF_SURF_DOWNLOAD], c[NV2A_PROF_SURF_UPLOAD],
             c[NV2A_PROF_TEX_UPLOAD], waits);
    Write(buf);

    // The stopwatches (hw/xbox/xemu-timing.h): ms per second, and times.
    double ms[XT__COUNT];
    unsigned int times[XT__COUNT];
    for (int i = 0; i < XT__COUNT; i++) {
        uint64_t ns = __atomic_load_n(&xemu_timing_ns[i], __ATOMIC_RELAXED);
        uint32_t count =
            __atomic_load_n(&xemu_timing_count[i], __ATOMIC_RELAXED);
        ms[i] = (ns - g_last_ns[i]) / 1e6 / interval;
        times[i] = count - g_last_count[i];
        g_last_ns[i] = ns;
        g_last_count[i] = count;
    }
    snprintf(buf, sizeof(buf),
             "           ms/s: GPU submit %.0f (%u), wait %.0f, one-off waits "
             "%.0f (%u), downloads %.0f (%u), idle %.0f | CPU run %.0f, idle "
             "%.0f | UI frame %.0f (%u), present %.0f | code thrown away %u, "
             "jump cache wipes %u\n",
             ms[XT_FINISH_SUBMIT], times[XT_FINISH_SUBMIT], ms[XT_FINISH_WAIT],
             ms[XT_AUX_WAIT], times[XT_AUX_WAIT], ms[XT_DOWNLOAD],
             times[XT_DOWNLOAD], ms[XT_GPU_IDLE], ms[XT_CPU_EXEC],
             ms[XT_CPU_IDLE], ms[XT_UI_FRAME], times[XT_UI_FRAME],
             ms[XT_UI_PRESENT], times[XT_TB_INVAL], times[XT_JC_FLUSH]);
    Write(buf);

    // A game that runs but draws nothing is the black screen case.
    if (running && !in_menu && frames == 0) {
        if (++g_idle_checks == 3 && !g_warned_idle) {
            g_warned_idle = true;
            Write("!! The game has drawn no frames for 15 s (black screen?)\n");
            fprintf(stderr, "XPSemu: no frames from the game for 15 s\n");
        }
    } else {
        g_idle_checks = 0;
    }
}
