/*
 * XOG: a Vulkan surface on the PS5's display, through VK_KHR_display as XPSemu
 * does: the driver reports one 3840x2160 mode. Our own code. GPL-3.0-or-later.
 */
#include <volk.h>

#include "native/core/nc.h"

/* The driver asks this before it takes the 120 Hz mode (linked with --wrap, tools/build.sh): no, unless this start
 * wants 120 (a mod that draws it). Anything else it asks, the system answers. */
int g_xog_want_120hz;
unsigned g_xog_refresh_mhz = 59940;
#define RX_MODE_HIGH_FRAME_RATE 15u
int __real_sceVideoOutIsOutputSupported(int handle, unsigned mode, const void *a, const void *b, const void *c);
int __wrap_sceVideoOutIsOutputSupported(int handle, unsigned mode, const void *a, const void *b, const void *c)
{
    if (mode == RX_MODE_HIGH_FRAME_RATE && !g_xog_want_120hz) {
        return -1;      /* not 0: the driver would go on and take 0 for a mode it has */
    }
    return __real_sceVideoOutIsOutputSupported(handle, mode, a, b, c);
}

VkResult rx_display_surface(VkInstance inst, VkPhysicalDevice phys, VkSurfaceKHR *out)
{
    uint32_t nd = 1, np = 1, nm = 1;
    VkDisplayPropertiesKHR dp;
    VkDisplayPlanePropertiesKHR pp;
    VkDisplayModePropertiesKHR mp;
    VkResult r = vkGetPhysicalDeviceDisplayPropertiesKHR(phys, &nd, &dp);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nd) {
        nc_log("xog-display", "no display: %d", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    r = vkGetPhysicalDeviceDisplayPlanePropertiesKHR(phys, &np, &pp);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !np) {
        nc_log("xog-display", "no plane: %d", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    r = vkGetDisplayModePropertiesKHR(phys, dp.display, &nm, &mp);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nm) {
        nc_log("xog-display", "no mode: %d", r);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkDisplaySurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_DISPLAY_SURFACE_CREATE_INFO_KHR,
        .displayMode = mp.displayMode,
        .planeIndex = 0,
        .planeStackIndex = pp.currentStackIndex,
        .transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .globalAlpha = 1.0f,
        .alphaMode = VK_DISPLAY_PLANE_ALPHA_OPAQUE_BIT_KHR,
        .imageExtent = mp.parameters.visibleRegion,
    };
    r = vkCreateDisplayPlaneSurfaceKHR(inst, &info, NULL, out);
    {
        extern unsigned g_xog_refresh_mhz;
        g_xog_refresh_mhz = mp.parameters.refreshRate;
    }
    nc_log("xog-display", "surface %ux%u at %u mHz: %d", mp.parameters.visibleRegion.width,
           mp.parameters.visibleRegion.height, mp.parameters.refreshRate, r);
    return r;
}
