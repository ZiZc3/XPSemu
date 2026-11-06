// The presenter's way to MihawkGen (ps5/framegen, GPL-3.0-or-later, Mihawk): swapchain.c is C, the library's interface C++.
#include <cstdio>
#include <vector>

#include "../framegen/include/framegen.h"

extern "C" void nc_log(const char *tag, const char *fmt, ...);

static fg_context *g_ctx;

extern "C" bool xog_fg_create(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h, uint32_t slots)
{
    char error[256] = "";
    FILE *f = fopen("/app0/framegen/network.fgw", "rb");
    if (!f) {
        nc_log("xog-fg", "no network file: frame generation off");
        return false;
    }
    std::vector<char> blob;
    char chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        blob.insert(blob.end(), chunk, chunk + n);
    }
    fclose(f);
    fg_create_info info = {};
    info.device = dev;
    info.physical_device = phys;
    info.width = w;
    info.height = h;
    info.slots = slots;
    info.output_format = VK_FORMAT_R8G8B8A8_UNORM;
    if (fg_context_create(&info, &g_ctx, error, sizeof(error)) != VK_SUCCESS) {
        nc_log("xog-fg", "couldn't start: %s", error);
        g_ctx = nullptr;
        return false;
    }
    if (fg_context_load_network(g_ctx, blob.data(), blob.size(), error, sizeof(error)) != VK_SUCCESS) {
        nc_log("xog-fg", "no network: %s", error);
        fg_context_destroy(g_ctx);
        g_ctx = nullptr;
        return false;
    }
    nc_log("xog-fg", "ready at %ux%u", w, h);
    return true;
}

extern "C" void xog_fg_destroy(void)
{
    if (g_ctx) {
        fg_context_destroy(g_ctx);
        g_ctx = nullptr;
    }
}

extern "C" void xog_fg_record(VkCommandBuffer cb, uint32_t slot, VkImageView prev, VkImageView next, float t, VkImageView out)
{
    fg_frame_pair pair = {};
    pair.prev_color = prev;
    pair.next_color = next;
    pair.t = t;
    fg_record_generate(g_ctx, cb, slot, FG_METHOD_NEURAL, &pair, out);
}
