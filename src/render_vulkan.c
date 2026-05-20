#include "render_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ME_HAVE_VULKAN

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

/* All Vulkan state lives here so render_vulkan.c can be self-contained.
   main.c only sees the opaque init/shutdown/is_active entry points. */
static struct {
    int            active;
    VkInstance     instance;
    VkPhysicalDevice phys;
    VkDevice       device;
    uint32_t       gfx_queue_family;
    VkQueue        gfx_queue;
    HWND           hwnd;
    unsigned       max_w, max_h;
    int            validation_enabled;
} g_vk;

static const char *vk_result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default: return "VK_ERROR_?";
    }
}

#define VK_CHECK(expr, what) do {                                         \
    VkResult _r = (expr);                                                 \
    if (_r != VK_SUCCESS) {                                               \
        fprintf(stderr, "[vk] %s failed: %s\n", (what), vk_result_str(_r));\
        goto fail;                                                        \
    }                                                                     \
} while (0)

static int layer_available(const char *name) {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS || count == 0)
        return 0;
    VkLayerProperties *props = calloc(count, sizeof(*props));
    if (!props) return 0;
    int found = 0;
    if (vkEnumerateInstanceLayerProperties(&count, props) == VK_SUCCESS) {
        for (uint32_t i = 0; i < count; i++) {
            if (strcmp(props[i].layerName, name) == 0) { found = 1; break; }
        }
    }
    free(props);
    return found;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug_messenger(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data,
    void *user)
{
    (void)types; (void)user;
    const char *tag =
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? "ERR" :
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? "WRN" :
        (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? "INF" : "DBG";
    fprintf(stderr, "[vk:%s] %s\n", tag, data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

static VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;

int me_vk_init(HWND hwnd, unsigned max_w, unsigned max_h) {
    memset(&g_vk, 0, sizeof(g_vk));
    g_vk.hwnd = hwnd;
    g_vk.max_w = max_w;
    g_vk.max_h = max_h;

    /* Validation is opt-in via env var. We never force it because the layer
       must be installed (Vulkan SDK) and adds significant overhead. */
    const char *vv = getenv("LAGGUELESS_VK_VALIDATE");
    int want_validation = (vv && vv[0] && vv[0] != '0');
    if (want_validation && !layer_available("VK_LAYER_KHRONOS_validation")) {
        fprintf(stderr, "[vk] LAGGUELESS_VK_VALIDATE=1 but VK_LAYER_KHRONOS_validation not installed\n");
        want_validation = 0;
    }
    g_vk.validation_enabled = want_validation;

    /* ---- instance --------------------------------------------------------- */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "laggueless",
        .applicationVersion = VK_MAKE_VERSION(0, 1, 0),
        .pEngineName = "laggueless",
        .engineVersion = VK_MAKE_VERSION(0, 1, 0),
        .apiVersion = VK_API_VERSION_1_3,
    };

    const char *inst_exts[8];
    uint32_t inst_ext_count = 0;
    inst_exts[inst_ext_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
    inst_exts[inst_ext_count++] = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
    if (want_validation) inst_exts[inst_ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;

    const char *inst_layers[2];
    uint32_t inst_layer_count = 0;
    if (want_validation) inst_layers[inst_layer_count++] = "VK_LAYER_KHRONOS_validation";

    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
        .enabledExtensionCount = inst_ext_count,
        .ppEnabledExtensionNames = inst_exts,
        .enabledLayerCount = inst_layer_count,
        .ppEnabledLayerNames = inst_layers,
    };

    VK_CHECK(vkCreateInstance(&ici, NULL, &g_vk.instance), "vkCreateInstance");

    /* Debug messenger (validation only). */
    if (want_validation) {
        PFN_vkCreateDebugUtilsMessengerEXT create_dbg =
            (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(g_vk.instance, "vkCreateDebugUtilsMessengerEXT");
        if (create_dbg) {
            VkDebugUtilsMessengerCreateInfoEXT dci = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = vk_debug_messenger,
            };
            create_dbg(g_vk.instance, &dci, NULL, &g_messenger);
        }
    }

    /* ---- physical device -------------------------------------------------- */
    uint32_t pcount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(g_vk.instance, &pcount, NULL), "vkEnumeratePhysicalDevices(count)");
    if (pcount == 0) {
        fprintf(stderr, "[vk] no Vulkan physical devices found\n");
        goto fail;
    }
    VkPhysicalDevice *phys = calloc(pcount, sizeof(*phys));
    if (!phys) goto fail;
    VkResult er = vkEnumeratePhysicalDevices(g_vk.instance, &pcount, phys);
    if (er != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkEnumeratePhysicalDevices(list) failed: %s\n", vk_result_str(er));
        free(phys);
        goto fail;
    }

    /* Pick a discrete GPU if available, else first device that has graphics. */
    int picked = -1;
    int picked_is_discrete = 0;
    uint32_t picked_qfam = 0;
    for (uint32_t i = 0; i < pcount; i++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(phys[i], &props);
        uint32_t qcount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qcount, NULL);
        if (qcount == 0) continue;
        VkQueueFamilyProperties *qprops = calloc(qcount, sizeof(*qprops));
        if (!qprops) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(phys[i], &qcount, qprops);
        int gfx_q = -1;
        for (uint32_t j = 0; j < qcount; j++) {
            if (qprops[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx_q = (int)j; break; }
        }
        free(qprops);
        if (gfx_q < 0) continue;
        int is_discrete = (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU);
        if (picked < 0 || (is_discrete && !picked_is_discrete)) {
            picked = (int)i;
            picked_is_discrete = is_discrete;
            picked_qfam = (uint32_t)gfx_q;
            if (is_discrete) break;
        }
    }
    if (picked < 0) {
        fprintf(stderr, "[vk] no graphics-capable Vulkan device found\n");
        free(phys);
        goto fail;
    }
    g_vk.phys = phys[picked];
    g_vk.gfx_queue_family = picked_qfam;
    free(phys);

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_vk.phys, &props);
    printf("[vk] adapter: %s (Vulkan %u.%u.%u, qfam=%u%s)\n",
           props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion),
           g_vk.gfx_queue_family,
           g_vk.validation_enabled ? ", validation on" : "");

    /* ---- logical device --------------------------------------------------- */
    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_vk.gfx_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &qprio,
    };
    const char *dev_exts[2];
    uint32_t dev_ext_count = 0;
    dev_exts[dev_ext_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = dev_ext_count,
        .ppEnabledExtensionNames = dev_exts,
    };
    VK_CHECK(vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.device), "vkCreateDevice");

    vkGetDeviceQueue(g_vk.device, g_vk.gfx_queue_family, 0, &g_vk.gfx_queue);

    g_vk.active = 1;
    return 0;

fail:
    me_vk_shutdown();
    return -1;
}

void me_vk_shutdown(void) {
    if (g_vk.device) {
        vkDeviceWaitIdle(g_vk.device);
        vkDestroyDevice(g_vk.device, NULL);
        g_vk.device = VK_NULL_HANDLE;
    }
    if (g_messenger && g_vk.instance) {
        PFN_vkDestroyDebugUtilsMessengerEXT destroy_dbg =
            (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(g_vk.instance, "vkDestroyDebugUtilsMessengerEXT");
        if (destroy_dbg) destroy_dbg(g_vk.instance, g_messenger, NULL);
        g_messenger = VK_NULL_HANDLE;
    }
    if (g_vk.instance) {
        vkDestroyInstance(g_vk.instance, NULL);
        g_vk.instance = VK_NULL_HANDLE;
    }
    g_vk.active = 0;
}

int me_vk_is_active(void) { return g_vk.active; }

#else  /* !ME_HAVE_VULKAN ---------------------------------------------------- */

int me_vk_init(HWND hwnd, unsigned max_w, unsigned max_h) {
    (void)hwnd; (void)max_w; (void)max_h;
    return -1;
}
void me_vk_shutdown(void) {}
int  me_vk_is_active(void) { return 0; }

#endif
