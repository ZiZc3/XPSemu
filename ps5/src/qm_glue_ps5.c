/* XOG: the game's Vulkan, for the quick menu. Its own code. GPL-3.0-or-later. */
#include "hw/xbox/hle/base.h"
#include "hw/xbox/hle/hle.h"
#include "hw/xbox/hle/vk/back_int.h"

extern VkFormat hle_present_format(void);

void xog_qm_vk(VkInstance *inst, VkPhysicalDevice *phys, VkDevice *dev, VkQueue *queue, uint32_t *family, VkFormat *fmt)
{
    *inst = g_hvk.inst;
    *phys = g_hvk.phys;
    *dev = g_hvk.dev;
    *queue = g_hvk.queue;
    *family = g_hvk.qfam;
    *fmt = hle_present_format();
}
