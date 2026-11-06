/*
 * hakuX native core, app front end — Vulkan layers over the custom driver
 * (debug). adrenotools hands the core Turnip's own vkGetInstanceProcAddr,
 * past Android's Vulkan loader, so the GPU debug-layer settings never reach
 * it. With debug.hakux.vkvalidation=1 at launch the Khronos validation
 * layer (debug APKs ship it beside the core) goes in front of the driver
 * here, which does the loader's part: the layer's link in the instance and
 * device create infos. A loader also writes its dispatch table into the
 * first word of each dispatchable object, which the layer files its state
 * under; Turnip reads that word itself (a key written there crashed its
 * vkGetInstanceProcAddr), so the driver's value stays: one for every object,
 * which the layer's separate instance and device maps take, the core making
 * one instance and one device.
 */
#ifndef VK_NO_PROTOTYPES                /* volk's targets define it */
#define VK_NO_PROTOTYPES
#endif
#include "native/app/app.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include "hw/xbox/hle/prop.h"
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define LAYER_LIB "libVkLayer_khronos_validation.so"

static PFN_vkGetInstanceProcAddr g_drv_gipa, g_layer_gipa;
static VkInstance g_inst;

/* objects the layer makes itself keep the driver's word too */
static VkResult VKAPI_CALL set_instance_data(VkInstance inst, void *obj)
{
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL set_device_data(VkDevice dev, void *obj)
{
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_instance(const VkInstanceCreateInfo *ci,
                                           const VkAllocationCallbacks *a, VkInstance *out)
{
    VkLayerInstanceLink link = { .pfnNextGetInstanceProcAddr = g_drv_gipa };
    VkLayerInstanceCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, .pNext = ci->pNext,
        .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &link };
    VkLayerInstanceCreateInfo ld = {
        .sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, .pNext = &li,
        .function = VK_LOADER_DATA_CALLBACK, .u.pfnSetInstanceLoaderData = set_instance_data };
    VkInstanceCreateInfo c = *ci;
    c.pNext = &ld;
    VkResult r = ((PFN_vkCreateInstance)g_layer_gipa(NULL, "vkCreateInstance"))(&c, a, out);
    if (r == VK_SUCCESS) {
        g_inst = *out;
    }
    return r;
}

static VkResult VKAPI_CALL create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci,
                                         const VkAllocationCallbacks *a, VkDevice *out)
{
    VkLayerDeviceLink link = {
        .pfnNextGetInstanceProcAddr = g_drv_gipa,
        .pfnNextGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)g_drv_gipa(g_inst, "vkGetDeviceProcAddr") };
    VkLayerDeviceCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .pNext = ci->pNext,
        .function = VK_LAYER_LINK_INFO, .u.pLayerInfo = &link };
    VkLayerDeviceCreateInfo ld = {
        .sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, .pNext = &li,
        .function = VK_LOADER_DATA_CALLBACK, .u.pfnSetDeviceLoaderData = set_device_data };
    VkDeviceCreateInfo c = *ci;
    c.pNext = &ld;
    return ((PFN_vkCreateDevice)g_layer_gipa(g_inst, "vkCreateDevice"))(pd, &c, a, out);
}

static PFN_vkVoidFunction VKAPI_CALL get_instance_proc(VkInstance inst, const char *name)
{
    if (!strcmp(name, "vkGetInstanceProcAddr")) {
        return (PFN_vkVoidFunction)get_instance_proc;
    }
    if (!strcmp(name, "vkCreateInstance")) {
        return (PFN_vkVoidFunction)create_instance;
    }
    if (!strcmp(name, "vkCreateDevice")) {
        return (PFN_vkVoidFunction)create_device;
    }
    if (!inst) {                        /* the driver's extensions, layers, version */
        return g_drv_gipa(NULL, name);
    }
    return g_layer_gipa(inst, name);
}

void *nc_vk_layers(void *drv_gipa)
{
    /* HAKUX_VKVALIDATION=1 too: a run that reads no properties (prop.h) */
    char v[PROP_VALUE_MAX] = "";
    const char *e = getenv("HAKUX_VKVALIDATION");
    if (!(e && !strcmp(e, "1")) && (hle_prop_get("debug.hakux.vkvalidation", v) <= 0 || !strcmp(v, "0"))) {
        return drv_gipa;
    }
    void *h = dlopen(LAYER_LIB, RTLD_NOW | RTLD_LOCAL);
    PFN_vkNegotiateLoaderLayerInterfaceVersion neg =
        h ? (PFN_vkNegotiateLoaderLayerInterfaceVersion)dlsym(h, "vkNegotiateLoaderLayerInterfaceVersion")
          : NULL;
    VkNegotiateLayerInterface ni = { .sType = LAYER_NEGOTIATE_INTERFACE_STRUCT,
                                     .loaderLayerInterfaceVersion = 2 };
    if (!neg || neg(&ni) != VK_SUCCESS || !ni.pfnGetInstanceProcAddr) {
        nc_log("hakuX-app", "Vulkan validation: %s not loaded (%s): none", LAYER_LIB,
               h ? "no layer interface" : dlerror());
        return drv_gipa;
    }
    g_drv_gipa = (PFN_vkGetInstanceProcAddr)drv_gipa;
    g_layer_gipa = ni.pfnGetInstanceProcAddr;
    nc_log("hakuX-app", "Vulkan validation: %s over the custom driver (layer interface %u)",
           LAYER_LIB, ni.loaderLayerInterfaceVersion);
    return (void *)get_instance_proc;
}
