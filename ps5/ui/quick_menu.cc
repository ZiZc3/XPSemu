// XOG: the quick menu. The touchpad over a running game: the game pauses, a panel comes in.
// Closed, nothing of this runs: the controller's thread looks at one button, the picture's thread at
// one flag. Its Vulkan and its fonts are made the first time it opens, in the game's own device, and
// what it changes applies at once. GPL-3.0-or-later.
#include <pthread.h>
#include <sys/stat.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <time.h>
#include <vector>

#include <volk.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "game-profile.hh"
#include "game-patches.hh"
#include "xiso.hh"
#include "xog_ui.h"

#include "data/RobotoCondensed-Regular.ttf.h"
#include "blades-art.h"
#include <stb_image.h>
#include <math.h>

extern "C" {
// qm_glue_ps5.c: the game's Vulkan, as the menu needs it
void xog_qm_vk(VkInstance *inst, VkPhysicalDevice *phys, VkDevice *dev, VkQueue *queue, uint32_t *family, VkFormat *fmt);
void hle_vk_queue_lock(void);
void hle_vk_queue_unlock(void);
extern void (*hle_present_overlay)(VkCommandBuffer cb, uint32_t w, uint32_t h);
void nc_log(const char *tag, const char *fmt, ...);
void nc_pause_set(bool on);
void nc_audio_set_paused(bool on);
void nc_present_set_aspect(int mode);
void nc_present_set_upscale(int mode);
void nc_present_set_fxaa(bool on);
void nc_present_set_smaa(bool on);
void hle_pb_set_fps_cap(int fps);
int hle_present_framegen_shown(void);
void hle_present_set_framegen_mode(unsigned most);
void nc_present_fps_counter(bool on);
void rx_audio_volume(int tens);
void rx_back_to_dashboard(const char *why);
void rx_restart_game(void);
void hle_back_rtdump_next(void);
void hle_draw_psframe_next(void);
int rx_game_is_pal(void);
void nc_present_picture(float *sx, float *sy);
void nc_present_stats(float *fps, float *avg_ms, float *last_ms);
int sceKernelAvailableFlexibleMemorySize(size_t *available);
const char *nc_state_save(const char *path);
bool hle_present_current(VkImage *img, uint32_t *w, uint32_t *h);

volatile int g_qm_open;             // the picture's thread looks at this each refresh
volatile int g_qm_overlay;          // side art or the counter: it looks for them too
volatile uint64_t g_qm_paused_ms;   // when it paused the game
uint64_t rx_now_ms(void);
volatile uint32_t g_qm_serial;      // while open: each refresh is a new picture
volatile uint32_t g_qm_buttons;     // the controller, from its thread
volatile int g_qm_stick_y, g_qm_stick_x;
}

namespace {

enum Row { R_RESUME, R_SAVE, R_LOAD, R_RENAME, R_DELETE, R_VOLUME, R_FPS, R_SHAPE, R_UPSCALE, R_AA, R_FG, R_CAP, R_PAL60, R_UNLOCK60, R_RESTART, R_SAVEHOME, R_HOME, R__COUNT,
           R_G_STATES = 100, R_G_SETTINGS, R_G_PERF, R_G_LEAVE };
static const struct { const char *name; int rows[6], count; } kGroups[] = {
    { "Save states", { R_SAVE, R_LOAD, R_RENAME, R_DELETE }, 4 },
    { "Settings", { R_VOLUME, R_FPS, R_SHAPE, R_UPSCALE }, 4 },
    { "Performance", { R_UNLOCK60, R_FG, R_CAP, R_AA, R_PAL60 }, 5 },
    { "Leave the game", { R_RESTART, R_SAVEHOME, R_HOME }, 3 },
};
enum { SLOTS = 10, THUMB_W = 480, THUMB_H = 270 };
enum { B_OPTIONS = 0x8, B_TRIANGLE = 0x1000, B_SQUARE = 0x8000, B_UP = 0x10, B_RIGHT = 0x20, B_DOWN = 0x40, B_LEFT = 0x80, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_TOUCH = 0x00100000 };

struct Menu {
    bool ready = false, failed = false, opening = false, closing = false;
    double opened = 0, closed = 0;
    int row = R_RESUME;
    uint32_t prev = 0;
    int held_dir = 0;
    double next_repeat = 0;
    double armed_until = 0;
    int armed_row = -1;
    bool changed = false, took_frame = false;
    unsigned dirty = 0;                 // the rows changed since the last save (1 << R_*): only those go into the game's profile
    int group = -1;     // the group open
    int sides = 0;      // 0 off, 1 green, 2 orange
    int scale = 1;
    double next_list = 0, last_sample = -1, last_cpu = 0;
    int cpu_percent = -1, display_fps = 0, refreshes = 0;
    long free_mb = -1;
    VkDescriptorSet blades[2] = {};
    bool blades_tried = false;
    float panel_w = 0, panel_h = 0;
    // the game's start, line by line; what's shown comes from it
    std::vector<std::string> lines;
    std::string name, key;
    int aa = 0;     // 0 off, 1 FXAA, 2 SMAA, 3 SSAA
    bool fg_started = false;    // this start has frame generation
    int cap = 0;                // the FPS cap, 0: none
    int cap_started = 0;        // the one this start runs with
    bool kb_cap = false;        // the keyboard is typing it
    int volume = 10, fps = 1, shape = 0, upscale = 1;   // shape: 0 auto, 1 4:3, 2 16:9, 3 stretch
    ImFont *font = nullptr;
    // states
    int slot = 1;
    std::string names[SLOTS + 1];   // what the user called them
    bool names_read[SLOTS + 1] = {};
    bool kb = false, kb_lower = false;
    std::string kb_text;
    int kb_row = 1, kb_col = 0;
    std::vector<uint8_t> frame;     // the frame it opened on, THUMB_W x THUMB_H
    VkSampler sampler = VK_NULL_HANDLE;
    struct Card {
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        bool looked = false;
        std::string when;
    } cards[SLOTS + 1];
    volatile int busy = 0;          // 1 saving, 2 done
    std::string save_path, save_why;
    int save_slot = 0;
    bool save_home = false;
    std::string note;
    double note_until = 0;
} g;

// Off to on, or on to Off: the 120 Hz mode is taken at the start.
bool FgRestartDue()
{
    return (FrameGenMode(g.key) > 0) != g.fg_started;
}

// The frame rate mod that fits this disc, if one does (the start's line).
std::string ModAvail()
{
    for (const std::string &l : g.lines) {
        if (l.rfind("modavail=", 0) == 0) {
            return l.substr(9);
        }
    }
    return "";
}

std::vector<int> VisibleRows()
{
    std::vector<int> rows = { R_RESUME };
    for (int i = 0; i < 4; i++) {
        rows.push_back(R_G_STATES + i);
        for (int k = 0; i == g.group && k < kGroups[i].count; k++) {
            if ((kGroups[i].rows[k] != R_PAL60 || rx_game_is_pal()) &&
                (kGroups[i].rows[k] != R_UNLOCK60 || Unlock60Known(g.key) || !ModAvail().empty())) {
                rows.push_back(kGroups[i].rows[k]);
            }
        }
    }
    return rows;
}

void Note(const std::string &text, double now)
{
    g.note = text;
    g.note_until = now + 4;
}

double Now()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

PFN_vkVoidFunction LoadFn(const char *name, void *)
{
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkFormat fmt;
    xog_qm_vk(&inst, &phys, &dev, &queue, &family, &fmt);
    PFN_vkVoidFunction f = vkGetDeviceProcAddr(dev, name);
    return f ? f : vkGetInstanceProcAddr(inst, name);
}

void CheckVk(VkResult r)
{
    if (r < 0) {
        nc_log("xog-qm", "Vulkan error %d", r);
    }
}

bool Prepare()
{
    if (g.ready || g.failed) {
        return g.ready;
    }
    const double t0 = Now();
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkFormat fmt;
    xog_qm_vk(&inst, &phys, &dev, &queue, &family, &fmt);
    if (!dev || fmt == VK_FORMAT_UNDEFINED) {
        return false;   // no picture yet: again at the next refresh
    }
    volkLoadDevice(dev);    // the backend's calls; the game's go through its own table
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;
    io.LogFilename = NULL;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    config.OversampleH = config.OversampleV = 1;
    g.font = io.Fonts->AddFontFromMemoryTTF((void *)RobotoCondensed_Regular_data, RobotoCondensed_Regular_size, 76.0f, &config,
                                            io.Fonts->GetGlyphRangesDefault());
    ImGui_ImplVulkan_LoadFunctions(LoadFn);
    ImGui_ImplVulkan_InitInfo init = {};
    init.Instance = inst;
    init.PhysicalDevice = phys;
    init.Device = dev;
    init.QueueFamily = family;
    init.Queue = queue;
    init.DescriptorPoolSize = 32;
    init.MinImageCount = 2;
    init.ImageCount = 8;    // more than can be in flight: each frame's vertices stay until it's done
    init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    init.UseDynamicRendering = true;
    init.PipelineRenderingCreateInfo = {};
    init.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    static VkFormat color;
    color = fmt;
    init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &color;
    init.CheckVkResultFn = CheckVk;
    if (!ImGui_ImplVulkan_Init(&init)) {
        nc_log("xog-qm", "no renderer for the menu");
        g.failed = true;
        return false;
    }
    hle_vk_queue_lock();
    ImGui_ImplVulkan_CreateFontsTexture();  // submits on the game's queue
    hle_vk_queue_unlock();
    g.ready = true;
    nc_log("xog-qm", "ready in %.1f ms", (Now() - t0) * 1000);
    return true;
}

struct GameVk {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t family;
    VkFormat fmt;
    GameVk() { xog_qm_vk(&inst, &phys, &dev, &queue, &family, &fmt); }
};

uint32_t MemType(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return 0;
}

// One set of commands on the game's queue, waited for.
template <typename F> void OneShot(F record)
{
    GameVk v;
    static VkCommandPool pool;
    if (!pool) {
        VkCommandPoolCreateInfo pi = {};
        pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = v.family;
        vkCreateCommandPool(v.dev, &pi, NULL, &pool);
    }
    VkCommandBufferAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vkAllocateCommandBuffers(v.dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence;
    vkCreateFence(v.dev, &fi, NULL, &fence);
    VkSubmitInfo si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    hle_vk_queue_lock();
    vkQueueSubmit(v.queue, 1, &si, fence);
    hle_vk_queue_unlock();
    vkWaitForFences(v.dev, 1, &fence, VK_TRUE, 2000000000ull);
    vkDestroyFence(v.dev, fence, NULL);
    vkFreeCommandBuffers(v.dev, pool, 1, &cb);
}

void Barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags sa, VkAccessFlags da,
             VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

struct Staging {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void *map = nullptr;
};

bool MakeStaging(Staging *st, VkBufferUsageFlags usage, VkDeviceSize size = THUMB_W * THUMB_H * 4)
{
    GameVk v;
    VkBufferCreateInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(v.dev, &bi, NULL, &st->buf) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v.dev, st->buf, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(v.phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(v.dev, &ai, NULL, &st->mem) != VK_SUCCESS) {
        return false;
    }
    vkBindBufferMemory(v.dev, st->buf, st->mem, 0);
    vkMapMemory(v.dev, st->mem, 0, bi.size, 0, &st->map);
    return true;
}

void FreeStaging(Staging *st)
{
    GameVk v;
    vkDestroyBuffer(v.dev, st->buf, NULL);
    vkFreeMemory(v.dev, st->mem, NULL);
}

bool NewImage(VkImage *img, VkDeviceMemory *mem, VkImageUsageFlags usage, uint32_t w = THUMB_W, uint32_t h = THUMB_H)
{
    GameVk v;
    VkImageCreateInfo ii = {};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = { w, h, 1 };
    ii.mipLevels = ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(v.dev, &ii, NULL, img) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v.dev, *img, &req);
    VkMemoryAllocateInfo ai = {};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = MemType(v.phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(v.dev, &ai, NULL, mem) != VK_SUCCESS) {
        return false;
    }
    vkBindImageMemory(v.dev, *img, *mem, 0);
    return true;
}

// The frame on the screen, small, for a state saved from this menu.
void TakeFrame()
{
    VkImage src;
    uint32_t w, h;
    g.frame.clear();
    if (!hle_present_current(&src, &w, &h) || !src) {
        return;
    }
    GameVk v;
    VkImage small;
    VkDeviceMemory small_mem;
    Staging st;
    if (!NewImage(&small, &small_mem, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !MakeStaging(&st, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        return;
    }
    OneShot([&](VkCommandBuffer cb) {
        Barrier(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        Barrier(cb, small, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { (int32_t)w, (int32_t)h, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { THUMB_W, THUMB_H, 1 };
        vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, small, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        Barrier(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        Barrier(cb, small, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { THUMB_W, THUMB_H, 1 };
        vkCmdCopyImageToBuffer(cb, small, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, st.buf, 1, &copy);
    });
    g.frame.resize(THUMB_W * THUMB_H * 4);
    memcpy(g.frame.data(), st.map, g.frame.size());
    for (size_t i = 3; i < g.frame.size(); i += 4) {
        g.frame[i] = 255;
    }
    FreeStaging(&st);
    vkDestroyImage(v.dev, small, NULL);
    vkFreeMemory(v.dev, small_mem, NULL);
}

std::string StateDir()
{
    return "/data/xpsemu/states/" + (g.key.empty() ? std::string("game") : g.key);
}

std::string StatePath(int slot)
{
    return StateDir() + (slot == 0 ? "/continue" : "/slot" + std::to_string(slot));
}

// The user's name for a save, or "Save 3".
std::string SlotLabel(int slot)
{
    if (!g.names_read[slot]) {
        g.names_read[slot] = true;
        std::ifstream f(StatePath(slot) + ".name");
        std::getline(f, g.names[slot]);
    }
    return g.names[slot].empty() ? "Save " + std::to_string(slot) : g.names[slot];
}

void SlotNameSet(int slot, const std::string &name)
{
    g.names[slot] = name;
    g.names_read[slot] = true;
    if (name.empty()) {
        unlink((StatePath(slot) + ".name").c_str());
    } else {
        std::ofstream(StatePath(slot) + ".name") << name << "\n";
    }
}

void CardDrop(int slot)
{
    Menu::Card &c = g.cards[slot];
    GameVk v;
    if (c.img) {
        hle_vk_queue_lock();
        vkQueueWaitIdle(v.queue);   // a frame may still be drawing it
        hle_vk_queue_unlock();
        ImGui_ImplVulkan_RemoveTexture(c.set);
        vkDestroyImageView(v.dev, c.view, NULL);
        vkDestroyImage(v.dev, c.img, NULL);
        vkFreeMemory(v.dev, c.mem, NULL);
    }
    c = Menu::Card();
}

// A slot's card: when, and its picture, the first time it's looked at.
void CardLook(int slot)
{
    Menu::Card &c = g.cards[slot];
    if (c.looked) {
        return;
    }
    c.looked = true;
    const std::string path = StatePath(slot);
    struct stat st;
    if (stat((path + ".state").c_str(), &st) != 0) {
        return;
    }
    char when[64];
    const time_t t = st.st_mtime;
    strftime(when, sizeof(when), "%b %d, %H:%M", localtime(&t));
    c.when = when;
    std::vector<uint8_t> px(THUMB_W * THUMB_H * 4);
    FILE *f = fopen((path + ".thumb").c_str(), "rb");
    if (!f) {
        return;
    }
    const bool ok = fread(px.data(), 1, px.size(), f) == px.size();
    fclose(f);
    if (!ok) {
        return;
    }
    GameVk v;
    Staging sb;
    if (!NewImage(&c.img, &c.mem, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !MakeStaging(&sb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
        return;
    }
    memcpy(sb.map, px.data(), px.size());
    OneShot([&](VkCommandBuffer cb) {
        Barrier(cb, c.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { THUMB_W, THUMB_H, 1 };
        vkCmdCopyBufferToImage(cb, sb.buf, c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cb, c.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    FreeStaging(&sb);
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = c.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(v.dev, &vi, NULL, &c.view);
    if (!g.sampler) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(v.dev, &si, NULL, &g.sampler);
    }
    c.set = ImGui_ImplVulkan_AddTexture(g.sampler, c.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// A picture from memory, for ImGui to draw.
VkDescriptorSet TextureFrom(const uint8_t *px, uint32_t w, uint32_t h)
{
    GameVk v;
    VkImage img;
    VkDeviceMemory mem;
    Staging sb;
    const VkDeviceSize size = (VkDeviceSize)w * h * 4;
    if (!NewImage(&img, &mem, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, w, h) ||
        !MakeStaging(&sb, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, size)) {
        return VK_NULL_HANDLE;
    }
    memcpy(sb.map, px, size);
    OneShot([&](VkCommandBuffer cb) {
        Barrier(cb, img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy = {};
        copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        copy.imageExtent = { w, h, 1 };
        vkCmdCopyBufferToImage(cb, sb.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    });
    FreeStaging(&sb);
    VkImageView view;
    VkImageViewCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCreateImageView(v.dev, &vi, NULL, &view);
    if (!g.sampler) {
        VkSamplerCreateInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(v.dev, &si, NULL, &g.sampler);
    }
    return ImGui_ImplVulkan_AddTexture(g.sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// A save runs beside the menu: the game has to run on to its next stop, and the menu goes on showing.
void *SaveThread(void *)
{
    const std::string part = g.save_path + ".part";
    mkdir("/data/xpsemu/states", 0777);
    mkdir(StateDir().c_str(), 0777);
    const double t0 = Now();
    const char *err = nc_state_save(part.c_str());
    for (int tries = 0; err && strstr(err, "try again") && tries < 30; tries++) {
        usleep(100000);     // the game writing a file of its own
        err = nc_state_save(part.c_str());
    }
    if (err) {
        unlink(part.c_str());
        g.save_why = err;
    } else {
        rename(part.c_str(), (g.save_path + ".state").c_str());
        if (!g.frame.empty()) {
            FILE *f = fopen((g.save_path + ".thumb").c_str(), "wb");
            if (f) {
                fwrite(g.frame.data(), 1, g.frame.size(), f);
                fclose(f);
            }
        }
        g.save_why.clear();
    }
    nc_log("xog-qm", "state %s: %s in %.0f ms", g.save_path.c_str(), err ? err : "saved", (Now() - t0) * 1000);
    __atomic_store_n(&g.busy, 2, __ATOMIC_RELEASE);
    return NULL;
}

void SaveStart(int slot, bool home)
{
    if (g.busy) {
        return;
    }
    g.save_slot = slot;
    g.save_home = home;
    g.save_path = StatePath(slot);
    g.busy = 1;
    pthread_t t;
    if (pthread_create(&t, NULL, SaveThread, NULL) != 0) {
        g.busy = 0;
        return;
    }
    pthread_detach(t);
}

void Overlay(VkCommandBuffer cb, uint32_t, uint32_t)
{
    ImDrawData *dd = ImGui::GetDrawData();
    if (dd) {
        ImGui_ImplVulkan_RenderDrawData(dd, cb);
    }
}

void SetLine(const char *prefix, const std::string &value)
{
    const std::string want = std::string(prefix) + value;
    for (std::string &l : g.lines) {
        if (l.rfind(prefix, 0) == 0) {
            l = want;
            return;
        }
    }
    g.lines.insert(g.lines.begin() + (g.lines.empty() ? 0 : 1), want);
}

const char *ShapeOption(int s)
{
    return s == 2 ? "16:9" : s == 1 ? "4:3" : s == 3 ? "stretch" : "auto";
}

// This run's lines and this game's own settings follow what the menu changed.
void Remember()
{
    SetLine("aspect=", ShapeOption(g.shape));
    SetLine("upscale=", g.upscale ? "sgsr" : "bilinear");
    SetLine("aa=", g.aa == 1 ? "fxaa" : g.aa == 2 ? "smaa" : g.aa == 3 ? "ssaa" : "off");
    SetLine("fpscap=", std::to_string(g.cap));
    SetLine("volume=", std::to_string(g.volume));
    SetLine("fps=", std::to_string(g.fps));
    if (!g.changed || g.key.empty()) {
        return;
    }
    // Only what the player changed here becomes the game's own: the rest keeps following Settings
    // (all of it was written before, and a game's AA, upscaler, cap... stayed as they were then for good)
    GameProfile p = GameProfileLoad(g.key);
    const auto d = [](int row) { return (g.dirty >> row) & 1u; };
    if (d(R_VOLUME)) {
        p.value[GO_VOLUME] = g.volume;
    }
    if (d(R_FPS)) {
        p.value[GO_OVERLAY] = g.fps;
    }
    if (d(R_UPSCALE)) {
        p.value[GO_FILTER] = g.upscale ? CONFIG_UPSCALE_SHARP : CONFIG_UPSCALE_SMOOTH;
    }
    if (d(R_AA)) {
        p.value[GO_AA] = g.aa;
    }
    if (d(R_CAP)) {
        p.value[GO_FPS_CAP] = g.cap;
    }
    if (d(R_SHAPE)) {
        p.value[GO_FIT] = g.shape == 3 ? CONFIG_DISPLAY_UI_FIT_STRETCH : CONFIG_DISPLAY_UI_FIT_SCALE;
        if (g.shape != 3) {
            p.value[GO_ASPECT] = g.shape == 2 ? CONFIG_DISPLAY_UI_ASPECT_RATIO_16X9 :
                                 g.shape == 1 ? CONFIG_DISPLAY_UI_ASPECT_RATIO_4X3 : CONFIG_DISPLAY_UI_ASPECT_RATIO_AUTO;
        }
    }
    GameProfileSave(g.key, p);
    g.changed = false;
    g.dirty = 0;
}

void Close()
{
    Remember();
    g.closing = true;
    g.closed = Now();
    nc_log("xog-qm", "closed");
    if (getenv("XOG_DUMP")) {
        hle_back_rtdump_next();
        hle_draw_psframe_next();
        nc_log("xog-qm", "dump: the next frame");
    }
}

void Change(int dir)
{
    switch (g.row) {
    case R_SAVE:
    case R_LOAD:
    case R_RENAME:
    case R_DELETE:
        g.slot = (g.slot - 1 + SLOTS + dir) % SLOTS + 1;
        g.armed_row = -1;
        return;
    case R_VOLUME:
        g.dirty |= 1u << R_VOLUME;
        g.volume = std::clamp(g.volume + dir, 0, 10);
        rx_audio_volume(g.volume);
        break;
    case R_FPS:
        g.dirty |= 1u << R_FPS;
        g.fps = (g.fps + 3 + dir) % 3;
        g_qm_overlay = g.fps || g.sides;
        break;
    case R_SHAPE:
        g.dirty |= 1u << R_SHAPE;
        g.shape = (g.shape + 4 + dir) % 4;
        nc_present_set_aspect(g.shape);
        break;
    case R_UPSCALE:
        g.dirty |= 1u << R_UPSCALE;
        g.upscale = !g.upscale;
        nc_present_set_upscale(g.upscale ? 1 : 0);
        break;
    case R_CAP: {
        g.dirty |= 1u << R_CAP;
        static const int kCaps[5] = { 0, 30, 60, 90, -1 };   // -1: type it
        int at = 0;
        for (int i = 0; i < 4; i++) {
            at = kCaps[i] == g.cap ? i : at;
        }
        at = (at + 5 + dir) % 5;
        if (kCaps[at] < 0) {
            g.kb = true;
            g.kb_cap = true;
            g.kb_text.clear();
            g.kb_row = 0;
            g.kb_col = 0;
            return;
        }
        g.cap = kCaps[at];
        break;
    }
    case R_FG: {
        const int mode = (FrameGenMode(g.key) + 5 + dir) % 5;
        FrameGenSetMode(g.key, mode);
        hle_present_set_framegen_mode(mode > 1 ? (unsigned)mode : 0);
        g.armed_row = -1;
        return;
    }
    case R_AA:
        g.dirty |= 1u << R_AA;
        g.aa = (g.aa + 4 + dir) % 4;
        nc_present_set_fxaa(g.aa == 1);
        nc_present_set_smaa(g.aa == 2);
        break;
    default:
        return;
    }
    g.changed = true;
}

void Choose(double now)
{
    if (g.row >= R_G_STATES) {
        const int group = g.row - R_G_STATES;
        g.group = g.group == group ? -1 : group;
        g.armed_row = -1;
    } else if (g.row == R_RESUME) {
        Close();
    } else if (g.row == R_SAVE) {
        SaveStart(g.slot, false);
    } else if (g.row == R_SAVEHOME) {
        SaveStart(0, true);
    } else if (g.row == R_RENAME) {
        struct stat st;
        if (stat((StatePath(g.slot) + ".state").c_str(), &st) != 0) {
            Note("Save " + std::to_string(g.slot) + " is empty", now);
        } else {
            SlotLabel(g.slot);
            g.kb = true;
            g.kb_text = g.names[g.slot];
            g.kb_row = 1;
            g.kb_col = 0;
        }
    } else if (g.row == R_DELETE) {
        struct stat st;
        if (stat((StatePath(g.slot) + ".state").c_str(), &st) != 0) {
            Note("Save " + std::to_string(g.slot) + " is empty", now);
        } else if (g.armed_row != g.row || now > g.armed_until) {
            g.armed_row = g.row;
            g.armed_until = now + 3;
        } else {
            unlink((StatePath(g.slot) + ".state").c_str());
            unlink((StatePath(g.slot) + ".thumb").c_str());
            SlotNameSet(g.slot, "");
            CardDrop(g.slot);
            g.armed_row = -1;
            Note("Save " + std::to_string(g.slot) + " deleted", now);
        }
    } else if (g.row == R_RESTART || g.row == R_HOME || g.row == R_LOAD || g.row == R_PAL60 || g.row == R_UNLOCK60 || (g.row == R_FG && FgRestartDue()) ||
               (g.row == R_CAP && g.cap != g.cap_started)) {
        struct stat st;
        if (g.row == R_LOAD && stat((StatePath(g.slot) + ".state").c_str(), &st) != 0) {
            Note("Save " + std::to_string(g.slot) + " is empty", now);
            return;
        }
        // twice: each of these ends what's being played
        if (g.armed_row != g.row || now > g.armed_until) {
            g.armed_row = g.row;
            g.armed_until = now + 3;
            return;
        }
        Remember();
        if (g.row == R_RESTART || g.row == R_LOAD || g.row == R_PAL60 || g.row == R_UNLOCK60 || g.row == R_FG || g.row == R_CAP) {
            std::string all;
            if (g.row == R_UNLOCK60) {
                Unlock60Toggle(g.key);
            }
            if (g.row == R_PAL60) {
                Pal60Toggle(g.key);
            }
            for (const std::string &l : g.lines) {
                if (g.row == R_PAL60 && l.rfind("video=", 0) == 0) {
                    const unsigned long bits = strtoul(l.c_str() + 6, NULL, 16);
                    char v[32];
                    snprintf(v, sizeof(v), "video=%lx", Pal60On(g.key) ? bits | 0x400000ul : bits & ~0x400000ul);
                    all += std::string(v) + "\n";
                } else if ((l == "unlock60" || l.rfind("mod=", 0) == 0) && g.row == R_UNLOCK60) {
                    // goes by the switch, below
                } else if (l.rfind("framegen=", 0) == 0) {
                    // goes by the setting, below
                } else if (l.rfind("loadstate=", 0) != 0 && l != "relaunch") {
                    all += l + "\n";
                }
            }
            all += FrameGenLine(g.key);
            if (g.row == R_LOAD) {
                all += "loadstate=" + StatePath(g.slot) + ".state\n";
            }
            if (g.row == R_UNLOCK60 && Fps60Wanted(g.key)) {
                const std::string mod = ModAvail();
                all += mod.empty() ? std::string("unlock60\n") : "mod=" + mod + "\n";
            }
            const std::string tmp = "/data/xpsemu/launch.tmp", file = "/data/xpsemu/launch.txt";
            {
                std::ofstream f(tmp);
                f << all;
            }
            rename(tmp.c_str(), file.c_str());
            nc_log("xog-qm", "restarting the game");
            rx_restart_game();
        } else {
            nc_log("xog-qm", "back to home");
            rx_back_to_dashboard(NULL);
        }
    } else {
        Change(1);
    }
}

static const char *const kKbRows[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM .!" };
enum { KB_ROWS = 4, KB_COLS = 10, KB_MAX = 24 };

void KbInput(double now, uint32_t b, uint32_t pressed)
{
    if (pressed & B_CIRCLE) {       // as it was
        g.kb = false;
        g.kb_cap = false;
        return;
    }
    if ((pressed & B_OPTIONS) && g.kb_cap) {    // nothing or 0: no cap
        const int cap = std::clamp(atoi(g.kb_text.c_str()), 0, 1000);
        g.cap = cap > 0 && cap < 5 ? 5 : cap;
        g.changed = true;
        g.kb = false;
        g.kb_cap = false;
        return;
    }
    if (pressed & B_OPTIONS) {      // an empty name goes back to "Save 3"
        SlotNameSet(g.slot, g.kb_text);
        g.kb = false;
        return;
    }
    if (pressed & B_TRIANGLE) {
        g.kb_lower = !g.kb_lower;
    }
    if ((pressed & B_SQUARE) && !g.kb_text.empty()) {
        g.kb_text.pop_back();
    }
    if ((pressed & B_CROSS) && g.kb_text.size() < KB_MAX) {
        const char ch = kKbRows[g.kb_row][g.kb_col];
        g.kb_text += g.kb_lower && ch >= 'A' && ch <= 'Z' ? (char)(ch + 32) : ch;
    }
    const int sy = g_qm_stick_y, sx = g_qm_stick_x;
    const int dir = (b & B_UP) || sy < -60 ? 1 : (b & B_DOWN) || sy > 60 ? 2 : (b & B_LEFT) || sx < -60 ? 3 : (b & B_RIGHT) || sx > 60 ? 4 : 0;
    bool act = false;
    if (dir != g.held_dir) {
        g.held_dir = dir;
        g.next_repeat = now + 0.32;
        act = dir != 0;
    } else if (dir && now >= g.next_repeat) {
        g.next_repeat = now + 0.11;
        act = true;
    }
    if (act) {
        if (dir == 1) g.kb_row = (g.kb_row + KB_ROWS - 1) % KB_ROWS;
        if (dir == 2) g.kb_row = (g.kb_row + 1) % KB_ROWS;
        if (dir == 3) g.kb_col = (g.kb_col + KB_COLS - 1) % KB_COLS;
        if (dir == 4) g.kb_col = (g.kb_col + 1) % KB_COLS;
    }
}

void Input(double now)
{
    const uint32_t b = g_qm_buttons;
    const uint32_t pressed = b & ~g.prev;
    g.prev = b;
    if (g.kb) {
        KbInput(now, b, pressed);
        return;
    }
    if (pressed & B_TOUCH) {
        Close();
        return;
    }
    if (pressed & B_CIRCLE) {
        if (g.group >= 0) {     // the group closes first
            g.row = R_G_STATES + g.group;
            g.group = -1;
        } else {
            Close();
        }
        return;
    }
    if (pressed & B_CROSS) {
        Choose(now);
        if (g.closing) {
            return;
        }
    }
    const int sy = g_qm_stick_y, sx = g_qm_stick_x;
    int dir = (b & B_UP) || sy < -60 ? 1 : (b & B_DOWN) || sy > 60 ? 2 : (b & B_LEFT) || sx < -60 ? 3 : (b & B_RIGHT) || sx > 60 ? 4 : 0;
    bool act = false;
    if (dir != g.held_dir) {
        g.held_dir = dir;
        g.next_repeat = now + 0.32;
        act = dir != 0;
    } else if (dir && now >= g.next_repeat) {
        g.next_repeat = now + 0.11;
        act = true;
    }
    if (act) {
        if (dir <= 2) {
            const std::vector<int> rows = VisibleRows();
            int at = 0;
            for (size_t i = 0; i < rows.size(); i++) {
                if (rows[i] == g.row) {
                    at = (int)i;
                }
            }
            const int n = (int)rows.size();
            g.row = rows[(at + (dir == 1 ? n - 1 : 1)) % n];
        }
        if (dir == 3) Change(-1);
        if (dir == 4) Change(1);
        if (dir <= 2) g.armed_row = -1;
    }
}

ImU32 Col(int r, int g_, int b, float a)
{
    return IM_COL32(r, g_, b, (int)(255 * std::clamp(a, 0.0f, 1.0f)));
}

void Bar(ImDrawList *dl, ImVec2 a, ImVec2 b, float cut, ImU32 fill, ImU32 line)
{
    const ImVec2 p[6] = { ImVec2(a.x + cut, a.y), ImVec2(b.x, a.y), ImVec2(b.x, b.y - cut),
                          ImVec2(b.x - cut, b.y), ImVec2(a.x, b.y), ImVec2(a.x, a.y + cut) };
    dl->AddConvexPolyFilled(p, 6, fill);
    dl->AddPolyline(p, 6, line, ImDrawFlags_Closed, 2.5f);
}

// ---- beside and over the picture
VkDescriptorSet BladeTexture(const char *file, const unsigned char *data, int size)
{
    int w, h, channels;
    const std::string path = std::string("/data/xpsemu/") + file;
    unsigned char *px = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!px) {
        px = stbi_load_from_memory(data, size, &w, &h, &channels, 4);
    }
    if (!px) {
        return VK_NULL_HANDLE;
    }
    VkDescriptorSet set = TextureFrom(px, (uint32_t)w, (uint32_t)h);
    stbi_image_free(px);
    return set;
}

void DrawSides(ImDrawList *dl, float W, float H, float s)
{
    float sx, sy;
    nc_present_picture(&sx, &sy);
    const float x0 = W / 2 * (1 - sx), x1 = W / 2 * (1 + sx);
    if (x0 < 40 * s) {
        return;     // a picture that fills the screen
    }
    if (!g.blades_tried) {
        g.blades_tried = true;
        if (g.sides == 2) {
            g.blades[0] = BladeTexture("blades-left.png", kBladesLeft, sizeof(kBladesLeft));
            g.blades[1] = BladeTexture("blades-right.png", kBladesRight, sizeof(kBladesRight));
        } else {
            g.blades[0] = BladeTexture("blades-green-left.png", kBladesGreenLeft, sizeof(kBladesGreenLeft));
            g.blades[1] = BladeTexture("blades-green-right.png", kBladesGreenRight, sizeof(kBladesGreenRight));
        }
    }
    if (g.blades[0]) {
        dl->AddImage((ImTextureID)(uintptr_t)g.blades[0], ImVec2(0, 0), ImVec2(x0, H));
    }
    if (g.blades[1]) {
        dl->AddImage((ImTextureID)(uintptr_t)g.blades[1], ImVec2(x1, 0), ImVec2(W, H));
    }
    const float sw = 18 * s;   // a soft shadow where they meet the game
    dl->AddRectFilledMultiColor(ImVec2(x0 - sw, 0), ImVec2(x0, H), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 128),
                                IM_COL32(0, 0, 0, 128), IM_COL32(0, 0, 0, 0));
    dl->AddRectFilledMultiColor(ImVec2(x1, 0), ImVec2(x1 + sw, H), IM_COL32(0, 0, 0, 128), IM_COL32(0, 0, 0, 0),
                                IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 128));
}

void DrawCounter(ImDrawList *dl, float s, double now)
{
    if (g.last_sample < 0 || now - g.last_sample >= 1.0) {
        struct timespec ts;
        if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
            const double cpu = ts.tv_sec + ts.tv_nsec / 1e9;
            if (g.last_sample >= 0) {
                g.cpu_percent = (int)lround((cpu - g.last_cpu) / (now - g.last_sample) * 100);
                g.display_fps = (int)lround(g.refreshes / (now - g.last_sample));
            }
            g.last_cpu = cpu;
        }
        g.refreshes = 0;
        size_t available = 0;
        if (sceKernelAvailableFlexibleMemorySize(&available) == 0) {
            g.free_mb = (long)(available >> 20);
        }
        g.last_sample = now;
    }
    float fps = 0, avg_ms = 0, last_ms = 0;
    nc_present_stats(&fps, &avg_ms, &last_ms);
    std::vector<std::string> lines;
    char buf[96];
    const int fg_shown = hle_present_framegen_shown();
    snprintf(buf, sizeof(buf), "%d FPS%s", (int)(fps + 0.5f), fg_shown >= 0 ? " FG" : "");
    lines.push_back(buf);
    if (g.fps == CONFIG_FPS_FULL && fg_shown >= 0) {
        snprintf(buf, sizeof(buf), "Shown  %d FPS", fg_shown);
        lines.push_back(buf);
    }
    if (g.fps == CONFIG_FPS_FULL) {
        snprintf(buf, sizeof(buf), "Frame time  %.0f ms", avg_ms);
        lines.push_back(buf);
        snprintf(buf, sizeof(buf), "Display  %d FPS", g.display_fps);
        lines.push_back(buf);
        if (g.cpu_percent >= 0) {
            snprintf(buf, sizeof(buf), "CPU  %d%% of a core", g.cpu_percent);
            lines.push_back(buf);
        }
        if (g.free_mb >= 0) {
            snprintf(buf, sizeof(buf), "Free memory  %ld MB", g.free_mb);
            lines.push_back(buf);
        }
        snprintf(buf, sizeof(buf), "Xbox 64 MB  -  x%d resolution", g.scale);
        lines.push_back(buf);
    }
    ImFont *f = g.font;
    const float size = 28 * s, pad = 14 * s, line = 34 * s;
    float w = 0;
    for (const std::string &l : lines) {
        w = std::max(w, f->CalcTextSizeA(size, FLT_MAX, 0, l.c_str()).x);
    }
    const ImVec2 p0(30 * s, 30 * s), p1(p0.x + w + 2 * pad, p0.y + lines.size() * line + 2 * pad - 6 * s);
    dl->AddRectFilled(p0, p1, IM_COL32(0, 20, 0, 153), 10 * s);
    // Green at full speed (many Xbox games run at 30), yellow when slow, red when very slow.
    const ImU32 col = fps >= 28 ? IM_COL32(205, 245, 45, 255) : fps >= 18 ? IM_COL32(255, 210, 60, 255) : IM_COL32(255, 90, 70, 255);
    for (size_t i = 0; i < lines.size(); i++) {
        dl->AddText(f, size, ImVec2(p0.x + pad, p0.y + pad + i * line), i == 0 ? col : IM_COL32(255, 255, 255, 230), lines[i].c_str());
    }
}

void Draw(double now, float k)
{
    ImGuiIO &io = ImGui::GetIO();
    const float W = io.DisplaySize.x, H = io.DisplaySize.y, s = H / 1080.0f;
    ImDrawList *dl = ImGui::GetBackgroundDrawList();
    const float ease = 1 - (1 - k) * (1 - k);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(W, H), Col(0, 0, 0, 0.6f * ease));

    // In the middle of the screen, growing out from it, as big as what's open.
    const std::vector<int> rows = VisibleRows();
    const bool states = g.group == 0;
    const float grow = 0.86f + 0.14f * ease, al = ease;
    const float u = s * grow;
    const float want_w = states ? 1240.0f : 760.0f, want_h = 106.0f + rows.size() * 74.0f + 62.0f;
    if (!g.panel_w) {
        g.panel_w = want_w;
        g.panel_h = want_h;
    }
    g.panel_w += (want_w - g.panel_w) * 0.3f;
    g.panel_h += (want_h - g.panel_h) * 0.3f;
    const float pw = g.panel_w * u, ph = g.panel_h * u;
    const ImVec2 c(W / 2, H / 2);
    const float px = c.x - pw / 2, py = c.y - ph / 2;
    Bar(dl, ImVec2(px, py), ImVec2(px + pw, py + ph), 34 * u, Col(6, 28, 4, 0.97f * al), Col(205, 245, 45, 0.9f * al));

    ImFont *f = g.font;
    const float m = 40 * u;
    float y = py + 30 * u;
    dl->AddText(f, 50 * u, ImVec2(px + m, y), Col(150, 235, 60, al), "XPSemu");
    std::string title = g.name;
    if (title.size() > 34) {
        title = title.substr(0, 31) + "...";
    }
    const ImVec2 ns = f->CalcTextSizeA(30 * u, FLT_MAX, 0, title.c_str());
    dl->AddText(f, 30 * u, ImVec2(px + pw - m - ns.x, y + 14 * u), Col(255, 255, 255, 0.8f * al), title.c_str());
    y += 76 * u;

    char value[48];
    const float bh = 62 * u, gap = 74 * u, bw = 680 * u, indent = 34 * u;
    for (int i : rows) {
        const char *name = "";
        value[0] = 0;
        const bool armed = g.armed_row == i && now <= g.armed_until;
        const bool head = i >= R_G_STATES;
        if (head) {
            name = kGroups[i - R_G_STATES].name;
            snprintf(value, sizeof(value), "%s", i - R_G_STATES == g.group ? "v" : ">");
        }
        switch (i) {
        case R_RESUME: name = "Resume"; break;
        case R_SAVE: name = "Save state"; snprintf(value, sizeof(value), "%s", SlotLabel(g.slot).c_str()); break;
        case R_LOAD: name = armed ? "Press again to load" : "Load state"; snprintf(value, sizeof(value), "%s", SlotLabel(g.slot).c_str()); break;
        case R_RENAME: name = "Name this save"; snprintf(value, sizeof(value), "%s", SlotLabel(g.slot).c_str()); break;
        case R_DELETE: name = armed ? "Press again to delete" : "Delete state"; snprintf(value, sizeof(value), "%s", SlotLabel(g.slot).c_str()); break;
        case R_VOLUME: name = "Volume"; snprintf(value, sizeof(value), "%d%%", g.volume * 10); break;
        case R_FPS: name = "FPS counter"; snprintf(value, sizeof(value), "%s", g.fps == 2 ? "Full" : g.fps ? "FPS" : "Off"); break;
        case R_SHAPE: {
            static const char *const shapes[] = { "Auto", "4:3", "16:9", "Stretch" };
            name = "Screen shape";
            snprintf(value, sizeof(value), "%s", shapes[g.shape]);
            break;
        }
        case R_UPSCALE: name = "Upscaling"; snprintf(value, sizeof(value), "%s", g.upscale ? "Sharp" : "Smooth"); break;
        case R_AA:
            name = "Anti-aliasing";
            snprintf(value, sizeof(value), "%s", g.aa == 1 ? "FXAA" : g.aa == 2 ? "SMAA" : g.aa != 3 ? "Off" :
                     g.scale < 6 ? "SSAA (next start)" : "SSAA");
            break;
        case R_CAP:
            name = armed ? "Press again: the game restarts" : "FPS cap";
            snprintf(value, sizeof(value), g.cap > 0 ? "%d FPS%s" : "Off%s", g.cap > 0 ? g.cap : 0,
                     g.cap != g.cap_started && !armed ? " (Cross: restart)" : "");
            if (g.cap <= 0) {
                snprintf(value, sizeof(value), "Off%s", g.cap != g.cap_started && !armed ? " (Cross: restart)" : "");
            }
            break;
        case R_FG:
            name = armed ? "Press again: the game restarts" : "Frame Generation";
        {
            static const char *const kFg[5] = { "Off", "Auto", "x2", "x3", "x4" };
            const int mode = FrameGenMode(g.key);
            snprintf(value, sizeof(value), "%s%s", kFg[mode >= 0 && mode < 5 ? mode : 0], FgRestartDue() && !armed ? " (Cross: restart)" : "");
        }
            break;
        case R_PAL60:
            name = armed ? "Press again: the game restarts" : "PAL 60 Hz";
            snprintf(value, sizeof(value), "%s", Pal60On(g.key) ? "On" : "Off");
            break;
        case R_UNLOCK60:
            name = armed ? "Press again: the game restarts" : "FPS Patch";
            snprintf(value, sizeof(value), "%s", Fps60Wanted(g.key) ? "On" : "Off");
            break;
        case R_RESTART: name = armed ? "Press again to restart" : "Restart game"; break;
        case R_SAVEHOME: name = "Save and go home"; break;
        case R_HOME: name = armed ? "Press again: back to home" : "Back to home"; break;
        }
        const bool sel = i == g.row;
        const float in = head || i == R_RESUME ? 0 : indent;
        const ImVec2 a(px + m + in, y), b(px + m + bw, y + bh);
        Bar(dl, a, b, 12 * u, sel ? Col(205, 245, 45, al) : Col(12, 45, 6, 0.8f * al), sel ? Col(255, 255, 255, 0.9f * al) : Col(70, 170, 30, 0.9f * al));
        const ImU32 text = sel ? Col(12, 45, 6, al) : Col(150, 235, 60, al);
        dl->AddText(f, 36 * u, ImVec2(a.x + 24 * u, a.y + 12 * u), text, name);
        if (value[0]) {
            char shown[64];
            snprintf(shown, sizeof(shown), sel && !head ? "<  %s  >" : "%s", value);
            const ImVec2 ts = f->CalcTextSizeA(36 * u, FLT_MAX, 0, shown);
            dl->AddText(f, 36 * u, ImVec2(b.x - ts.x - 24 * u, a.y + 12 * u), sel ? Col(12, 45, 6, al) : Col(255, 255, 255, 0.9f * al), shown);
        }
        y += gap;
    }
    dl->AddText(f, 26 * u, ImVec2(px + m, py + ph - 48 * u), Col(255, 255, 255, 0.65f * al), "Cross: choose     Circle: back     Touchpad: resume");

    // The slot's card beside the list, with the save states open.
    if (states && g.panel_w > 1100) {
        const float cx = px + m + bw + 36 * u, cw = pw - (cx - px) - m, ch = cw * 9 / 16;
        const float cy = py + 106 * u;
        const Menu::Card &card = g.cards[g.slot];
        dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + cw, cy + ch), Col(0, 0, 0, 0.5f * al));
        if (card.set) {
            dl->AddImage((ImTextureID)(uintptr_t)card.set, ImVec2(cx, cy), ImVec2(cx + cw, cy + ch), ImVec2(0, 0), ImVec2(1, 1), Col(255, 255, 255, al));
        }
        dl->AddRect(ImVec2(cx, cy), ImVec2(cx + cw, cy + ch), Col(70, 170, 30, al), 0, 0, 2.5f);
        char slot[32];
        snprintf(slot, sizeof(slot), "%s", SlotLabel(g.slot).c_str());
        dl->AddText(f, 36 * u, ImVec2(cx, cy + ch + 14 * u), Col(205, 245, 45, al), slot);
        dl->AddText(f, 30 * u, ImVec2(cx, cy + ch + 56 * u), Col(255, 255, 255, 0.8f * al), card.when.empty() ? "Empty" : card.when.c_str());
    }
    if (g.kb) {     // naming a save, over the menu
        const float kw = 1000 * u, kh = 560 * u, kx = c.x - kw / 2, ky = c.y - kh / 2;
        Bar(dl, ImVec2(kx, ky), ImVec2(kx + kw, ky + kh), 28 * u, Col(6, 28, 4, al), Col(205, 245, 45, al));
        const std::string shown = g.kb_text + "_";
        dl->AddText(f, 44 * u, ImVec2(kx + 40 * u, ky + 30 * u), Col(255, 255, 255, al), shown.c_str());
        const float cell = 92 * u, gx = kx + 40 * u, gy = ky + 100 * u;
        char ch[2] = { 0, 0 };
        for (int r = 0; r < KB_ROWS; r++) {
            for (int col = 0; col < KB_COLS; col++) {
                const bool on = r == g.kb_row && col == g.kb_col;
                const ImVec2 a(gx + col * cell, gy + r * cell), z(a.x + cell - 8 * u, a.y + cell - 8 * u);
                Bar(dl, a, z, 10 * u, on ? Col(205, 245, 45, al) : Col(12, 45, 6, al), on ? Col(255, 255, 255, al) : Col(70, 170, 30, al));
                const char key = kKbRows[r][col];
                ch[0] = g.kb_lower && key >= 'A' && key <= 'Z' ? (char)(key + 32) : key;
                dl->AddText(f, 44 * u, ImVec2(a.x + 28 * u, a.y + 20 * u), on ? Col(12, 45, 6, al) : Col(150, 235, 60, al), key == ' ' ? "_" : ch);
            }
        }
        dl->AddText(f, 28 * u, ImVec2(kx + 40 * u, ky + kh - 62 * u), Col(255, 255, 255, 0.85f * al),
                    "Cross type   Square delete   Triangle a/A   Options done   Circle cancel");
    }
    // What's going on, wherever it's asked from.
    const float ny = states && g.panel_w > 1100 ? py + 106 * u + (pw - 756 * u - m) * 9 / 16 + 110 * u : py + ph + 18 * u;
    const float nx = states && g.panel_w > 1100 ? px + m + bw + 36 * u : px + m;
    if (g.busy == 1) {
        dl->AddText(f, 34 * u, ImVec2(nx, ny), Col(205, 245, 45, al), "Saving...");
    } else if (now < g.note_until) {
        dl->AddText(f, 30 * u, ImVec2(nx, ny), Col(205, 245, 45, al), g.note.c_str(), NULL, 460 * u);
    }
}

}  // namespace

extern "C" {

// The game's start again, for the XBE it just asked for (main_ps5.c).
void xog_qm_write_relaunch(void)
{
    std::string all;
    for (const std::string &l : g.lines) {
        if (l.rfind("loadstate=", 0) != 0 && l.rfind("poke=", 0) != 0 && l != "relaunch") {
            all += l + "\n";
        }
    }
    all += "relaunch\n";
    // The next XBE's own patches, if it's on the disc.
    std::string next;
    {
        char page[8 + 520 + 1] = {};
        FILE *r = fopen("/data/xpsemu/relaunch.bin", "rb");
        if (r) {
            if (fread(page, 1, 8 + 520, r) != 8 + 520) {
                page[8] = 0;
            }
            fclose(r);
        }
        // the device, then ';' (XAPI's) or a backslash, then the file
        static const char cd[] = "\\Device\\CdRom0";
        if (!strncasecmp(page + 8, cd, sizeof(cd) - 1) && (page[8 + sizeof(cd) - 1] == ';' || page[8 + sizeof(cd) - 1] == '\\')) {
            next = page + 8 + sizeof(cd);
            std::replace(next.begin(), next.end(), ';', '\\');
        }
    }
    XisoInfo info;
    if (!next.empty() && !g.lines.empty() && XisoReadInfoOf(g.lines[0].c_str(), next, &info)) {
        xemu_settings_load();
        std::string pokes;
        bool wide = false;
        const std::string log = PatchesForStart(info.xbe_file.empty() ? g.lines[0] : info.xbe_file, info.xbe_offset, info.xbe_size,
                                                info.title_id, g_cfg.autopatch_widescreen, g_cfg.autopatch_60fps, &pokes, &wide);
        nc_log("xog", "next XBE %s: title %08x, %s", next.c_str(), info.title_id, pokes.empty() ? "no patches" : "patches found");
        if (!log.empty()) {
            nc_log("xog", "%s", log.c_str());
        }
        all += pokes;
        const size_t a = wide ? all.find("aspect=auto\n") : std::string::npos;
        if (a != std::string::npos) {
            all.replace(a, 12, "aspect=16:9\n");
        }
    }
    const std::string tmp = "/data/xpsemu/launch.tmp", file = "/data/xpsemu/launch.txt";
    {
        std::ofstream f(tmp);
        f << all;
    }
    rename(tmp.c_str(), file.c_str());
    nc_log("xog", "starting again for the title's next XBE");
}

// A line of the game's start (main_ps5.c).
void xog_qm_start_line(const char *line)
{
    std::string l = line;
    if (g.lines.empty()) {
        const size_t slash = l.find_last_of('/');
        g.name = slash == std::string::npos ? l : l.substr(slash + 1);
        const size_t dot = g.name.find_last_of('.');
        if (dot != std::string::npos) {
            g.name.resize(dot);
        }
    }
    if (l.rfind("key=", 0) == 0) {
        g.key = l.substr(4);
    } else if (l.rfind("name=", 0) == 0) {
        g.name = l.substr(5);
    } else if (l.rfind("volume=", 0) == 0) {
        g.volume = std::clamp(atoi(l.c_str() + 7), 0, 10);
    } else if (l.rfind("fps=", 0) == 0) {
        g.fps = std::clamp(atoi(l.c_str() + 4), 0, 2);
        g_qm_overlay = g.fps || g.sides;
    } else if (l.rfind("sides=", 0) == 0) {
        g.sides = std::clamp(atoi(l.c_str() + 6), 0, 2);
        g_qm_overlay = g.fps || g.sides;
    } else if (l.rfind("scale=", 0) == 0) {
        g.scale = atoi(l.c_str() + 6);
    } else if (l.rfind("fpscap=", 0) == 0) {
        g.cap = g.cap_started = atoi(l.c_str() + 7);
    } else if (l.rfind("framegen=", 0) == 0) {
        g.fg_started = true;
    } else if (l.rfind("aa=", 0) == 0) {
        g.aa = l == "aa=fxaa" ? 1 : l == "aa=smaa" ? 2 : l == "aa=ssaa" ? 3 : 0;
    } else if (l.rfind("upscale=", 0) == 0) {
        g.upscale = l == "upscale=sgsr";
    } else if (l.rfind("aspect=", 0) == 0) {
        const std::string v = l.substr(7);
        g.shape = v == "16:9" ? 2 : v == "4:3" ? 1 : v == "stretch" ? 3 : 0;
    }
    g.lines.push_back(l);
}

// The controller's thread: the touchpad, over a running game.
void xog_qm_request(void)
{
    if (!g_qm_open) {
        g.opening = true;
        g_qm_open = 1;
    }
}

// The picture's thread, each refresh while open, before the frame is drawn.
void xog_qm_frame(void)
{
    const double now = Now();
    if (g.opening) {
        g.opening = false;
        g_qm_paused_ms = rx_now_ms();
        nc_pause_set(true);
        nc_audio_set_paused(true);
        g.row = R_RESUME;
        g.group = -1;
        g.armed_row = -1;
        g.closing = false;
        g.prev = g_qm_buttons;  // the press that opened it isn't a press here
        g.held_dir = 0;
        g.opened = 0;
        nc_log("xog-qm", "open (the game paused)");
        g.took_frame = false;
    }
    bool open = g_qm_open != 0;
    if (!Prepare()) {
        if (g.failed) {     // no menu and no overlay: the game goes on
            if (open) {
                nc_pause_set(false);
                nc_audio_set_paused(false);
            }
            g_qm_open = 0;
            g_qm_overlay = 0;
        }
        return;
    }
    g.refreshes++;
    hle_present_overlay = Overlay;
    bool closed_now = false;
    float k = 0;
    if (open) {
        if (!g.took_frame) {
            g.took_frame = true;
            const double t0 = Now();
            TakeFrame();
            nc_log("xog-qm", "picture of the frame in %.1f ms", (Now() - t0) * 1000);
        }
        if (!g.opened) {
            g.opened = now;
        }
        if (g.closing && now - g.closed >= 0.10) {
            nc_pause_set(false);
            nc_audio_set_paused(false);
            g.closing = false;
            g_qm_open = 0;
            open = false;
            closed_now = true;
        }
    }
    if (open) {
        if (__atomic_load_n(&g.busy, __ATOMIC_ACQUIRE) == 2) {
            g.busy = 0;
            if (g.save_why.empty()) {
                CardDrop(g.save_slot);
                if (g.save_home) {
                    nc_log("xog-qm", "saved: back to home");
                    rx_back_to_dashboard(NULL);
                }
                Note(g.save_slot ? "Saved to slot " + std::to_string(g.save_slot) : std::string("Saved"), now);
            } else {
                Note("Not saved: " + g.save_why, now);
            }
        }
        if (!g.closing && !g.busy) {
            Input(now);
            CardLook(g.slot);
        }
        k = g.closing ? std::clamp((float)(1 - (now - g.closed) / 0.10), 0.0f, 1.0f)
                      : std::clamp((float)((now - g.opened) / 0.12), 0.0f, 1.0f);
    }
    if (!open && !closed_now && now < g.next_list) {
        return;     // the list as it is: side art and the counter change twice a second at most
    }
    g.next_list = now + 0.5;
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(3840, 2160);
    io.DeltaTime = 1.0f / 60;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    {
        const float s = io.DisplaySize.y / 1080.0f;
        ImDrawList *dl = ImGui::GetBackgroundDrawList();
        if (g.sides) {
            DrawSides(dl, io.DisplaySize.x, io.DisplaySize.y, s);
        }
        if (g.fps) {
            DrawCounter(dl, s, now);
        }
    }
    if (open) {
        Draw(now, k);
    }
    ImGui::Render();
    g_qm_serial = g_qm_serial + 1;
    if (closed_now && !g_qm_overlay) {
        hle_present_overlay = nullptr;
    }
}

}  // extern "C"
