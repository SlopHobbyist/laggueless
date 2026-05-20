#include "render_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ME_HAVE_VULKAN

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

/* Up to FRAMES_IN_FLIGHT real frames can be CPU-side prepared at the same
   time. Two is enough to overlap CPU with GPU without queueing extra latency.
   Real swapchain image count comes from the surface (typically 2 or 3). */
#define FRAMES_IN_FLIGHT 2
#define MAX_SWAPCHAIN_IMAGES 8

typedef struct {
    VkSemaphore image_available;
    VkSemaphore render_finished;
    VkFence     in_flight;
    VkCommandBuffer cmd;
} FrameSync;

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

    /* A3 additions */
    VkSurfaceKHR   surface;
    VkSwapchainKHR swapchain;
    VkFormat       sc_format;
    VkColorSpaceKHR sc_colorspace;
    VkExtent2D     sc_extent;
    uint32_t       sc_image_count;
    VkImage        sc_images[MAX_SWAPCHAIN_IMAGES];

    VkCommandPool  cmd_pool;
    FrameSync      frames[FRAMES_IN_FLIGHT];
    uint32_t       frame_index; /* which of FRAMES_IN_FLIGHT we're on */

    int            swapchain_needs_recreate;
} g_vk;

static VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;

static const char *vk_result_str(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
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

/* Pick a surface format that doesn't require sRGB conversion logic just yet.
   Prefer B8G8R8A8_UNORM (matches our CPU-side BGRX backbuffer); fall back to
   the first format the driver returns. */
static void pick_surface_format(VkSurfaceFormatKHR *out) {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &count, NULL);
    if (count == 0) {
        out->format = VK_FORMAT_B8G8R8A8_UNORM;
        out->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        return;
    }
    VkSurfaceFormatKHR *fmts = calloc(count, sizeof(*fmts));
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &count, fmts);
    *out = fmts[0];
    for (uint32_t i = 0; i < count; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            *out = fmts[i];
            break;
        }
    }
    free(fmts);
}

static void compute_extent(VkExtent2D *out) {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps);
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        *out = caps.currentExtent;
        return;
    }
    RECT rc; GetClientRect(g_vk.hwnd, &rc);
    uint32_t w = (uint32_t)(rc.right - rc.left);
    uint32_t h = (uint32_t)(rc.bottom - rc.top);
    if (w < caps.minImageExtent.width)  w = caps.minImageExtent.width;
    if (h < caps.minImageExtent.height) h = caps.minImageExtent.height;
    if (w > caps.maxImageExtent.width)  w = caps.maxImageExtent.width;
    if (h > caps.maxImageExtent.height) h = caps.maxImageExtent.height;
    out->width = w;
    out->height = h;
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed\n");
        return -1;
    }

    VkSurfaceFormatKHR sf;
    pick_surface_format(&sf);
    g_vk.sc_format = sf.format;
    g_vk.sc_colorspace = sf.colorSpace;

    compute_extent(&g_vk.sc_extent);
    if (g_vk.sc_extent.width == 0 || g_vk.sc_extent.height == 0) {
        /* Minimized window — defer creation. Mark needs_recreate. */
        g_vk.swapchain_needs_recreate = 1;
        return 0;
    }

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > MAX_SWAPCHAIN_IMAGES) want = MAX_SWAPCHAIN_IMAGES;

    VkSwapchainKHR old = g_vk.swapchain;

    /* A3 uses TRANSFER_DST so we can clear via vkCmdClearColorImage. A4 will
       add COLOR_ATTACHMENT once we switch to a render-pass draw. */
    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_vk.surface,
        .minImageCount = want,
        .imageFormat = g_vk.sc_format,
        .imageColorSpace = g_vk.sc_colorspace,
        .imageExtent = g_vk.sc_extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, /* A5 tunes this */
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VkResult r = vkCreateSwapchainKHR(g_vk.device, &sci, NULL, &g_vk.swapchain);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkCreateSwapchainKHR failed: %s\n", vk_result_str(r));
        return -1;
    }
    if (old != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(g_vk.device, old, NULL);
    }

    g_vk.sc_image_count = MAX_SWAPCHAIN_IMAGES;
    vkGetSwapchainImagesKHR(g_vk.device, g_vk.swapchain, &g_vk.sc_image_count, g_vk.sc_images);

    printf("[vk] swapchain: %ux%u, %u images, format=%d, FIFO\n",
           g_vk.sc_extent.width, g_vk.sc_extent.height, g_vk.sc_image_count, (int)g_vk.sc_format);

    g_vk.swapchain_needs_recreate = 0;
    return 0;
}

static void destroy_swapchain(void) {
    if (g_vk.swapchain) {
        vkDestroySwapchainKHR(g_vk.device, g_vk.swapchain, NULL);
        g_vk.swapchain = VK_NULL_HANDLE;
    }
    g_vk.sc_image_count = 0;
}

static int create_frame_sync(void) {
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_vk.gfx_queue_family,
    };
    if (vkCreateCommandPool(g_vk.device, &cpci, NULL, &g_vk.cmd_pool) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkCreateCommandPool failed\n");
        return -1;
    }
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkSemaphoreCreateInfo seci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        if (vkCreateSemaphore(g_vk.device, &seci, NULL, &g_vk.frames[i].image_available) != VK_SUCCESS) return -1;
        if (vkCreateSemaphore(g_vk.device, &seci, NULL, &g_vk.frames[i].render_finished) != VK_SUCCESS) return -1;
        if (vkCreateFence(g_vk.device, &fci, NULL, &g_vk.frames[i].in_flight) != VK_SUCCESS) return -1;
        VkCommandBufferAllocateInfo cbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_vk.cmd_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(g_vk.device, &cbi, &g_vk.frames[i].cmd) != VK_SUCCESS) return -1;
    }
    return 0;
}

static void destroy_frame_sync(void) {
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (g_vk.frames[i].image_available) vkDestroySemaphore(g_vk.device, g_vk.frames[i].image_available, NULL);
        if (g_vk.frames[i].render_finished) vkDestroySemaphore(g_vk.device, g_vk.frames[i].render_finished, NULL);
        if (g_vk.frames[i].in_flight) vkDestroyFence(g_vk.device, g_vk.frames[i].in_flight, NULL);
        memset(&g_vk.frames[i], 0, sizeof(g_vk.frames[i]));
    }
    if (g_vk.cmd_pool) {
        vkDestroyCommandPool(g_vk.device, g_vk.cmd_pool, NULL);
        g_vk.cmd_pool = VK_NULL_HANDLE;
    }
}

int me_vk_init(HWND hwnd, unsigned max_w, unsigned max_h) {
    memset(&g_vk, 0, sizeof(g_vk));
    g_vk.hwnd = hwnd;
    g_vk.max_w = max_w;
    g_vk.max_h = max_h;

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

    /* ---- surface (needed before picking device, to verify present support)  */
    VkWin32SurfaceCreateInfoKHR wci = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleW(NULL),
        .hwnd = hwnd,
    };
    VK_CHECK(vkCreateWin32SurfaceKHR(g_vk.instance, &wci, NULL, &g_vk.surface), "vkCreateWin32SurfaceKHR");

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
            if (!(qprops[j].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present_ok = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(phys[i], j, g_vk.surface, &present_ok);
            if (!present_ok) continue;
            gfx_q = (int)j; break;
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
        fprintf(stderr, "[vk] no graphics+present-capable Vulkan device found\n");
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

    /* ---- swapchain + per-frame sync -------------------------------------- */
    if (create_swapchain() != 0) goto fail;
    if (create_frame_sync() != 0) goto fail;

    g_vk.active = 1;
    return 0;

fail:
    me_vk_shutdown();
    return -1;
}

void me_vk_shutdown(void) {
    if (g_vk.device) {
        vkDeviceWaitIdle(g_vk.device);
    }
    destroy_frame_sync();
    destroy_swapchain();
    if (g_vk.device) {
        vkDestroyDevice(g_vk.device, NULL);
        g_vk.device = VK_NULL_HANDLE;
    }
    if (g_vk.surface && g_vk.instance) {
        vkDestroySurfaceKHR(g_vk.instance, g_vk.surface, NULL);
        g_vk.surface = VK_NULL_HANDLE;
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

/* Recreate the swapchain after a resize or OUT_OF_DATE. Caller must have
   already waited for the device to be idle on the failure path. */
static int recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.device);
    return create_swapchain();
}

int me_vk_present_clear(void) {
    if (!g_vk.active) return -1;

    /* Window minimized or otherwise zero-sized: skip this frame. We keep the
       fence states consistent by not advancing frame_index. */
    if (g_vk.swapchain_needs_recreate || g_vk.swapchain == VK_NULL_HANDLE) {
        if (recreate_swapchain() != 0) return -1;
        if (g_vk.swapchain == VK_NULL_HANDLE) return 0; /* still minimized */
    }

    FrameSync *f = &g_vk.frames[g_vk.frame_index];

    vkWaitForFences(g_vk.device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);

    uint32_t img_idx = 0;
    VkResult r = vkAcquireNextImageKHR(g_vk.device, g_vk.swapchain, UINT64_MAX,
                                       f->image_available, VK_NULL_HANDLE, &img_idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_vk.swapchain_needs_recreate = 1;
        return 0;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        fprintf(stderr, "[vk] vkAcquireNextImageKHR: %s\n", vk_result_str(r));
        return -1;
    }

    vkResetFences(g_vk.device, 1, &f->in_flight);

    /* Record: transition to TRANSFER_DST_OPTIMAL, clear, transition to PRESENT_SRC_KHR. */
    vkResetCommandBuffer(f->cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(f->cmd, &bi) != VK_SUCCESS) return -1;

    VkImageMemoryBarrier to_dst = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_vk.sc_images[img_idx],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(f->cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, NULL, 0, NULL, 1, &to_dst);

    /* Cornflower blue, but in linear-ish BGRA UNORM space the value still
       displays as a clearly-recognizable blue. */
    VkClearColorValue clear = { .float32 = { 0.39f, 0.58f, 0.93f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(f->cmd, g_vk.sc_images[img_idx],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear, 1, &range);

    VkImageMemoryBarrier to_present = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = 0,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_vk.sc_images[img_idx],
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(f->cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
        0, 0, NULL, 0, NULL, 1, &to_present);

    if (vkEndCommandBuffer(f->cmd) != VK_SUCCESS) return -1;

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &f->image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &f->cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &f->render_finished,
    };
    if (vkQueueSubmit(g_vk.gfx_queue, 1, &si, f->in_flight) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkQueueSubmit failed\n");
        return -1;
    }

    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &f->render_finished,
        .swapchainCount = 1,
        .pSwapchains = &g_vk.swapchain,
        .pImageIndices = &img_idx,
    };
    r = vkQueuePresentKHR(g_vk.gfx_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        g_vk.swapchain_needs_recreate = 1;
    } else if (r != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkQueuePresentKHR: %s\n", vk_result_str(r));
    }

    g_vk.frame_index = (g_vk.frame_index + 1) % FRAMES_IN_FLIGHT;
    return 0;
}

#else  /* !ME_HAVE_VULKAN ---------------------------------------------------- */

int me_vk_init(HWND hwnd, unsigned max_w, unsigned max_h) {
    (void)hwnd; (void)max_w; (void)max_h;
    return -1;
}
void me_vk_shutdown(void) {}
int  me_vk_is_active(void) { return 0; }
int  me_vk_present_clear(void) { return -1; }

#endif
