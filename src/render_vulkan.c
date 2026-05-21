#include "render_vulkan.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ME_HAVE_VULKAN

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include "shaders_vk_embedded.h"

#ifdef ME_HAVE_LSFG
#include "lsfg_bridge.h"
#endif

/* Up to FRAMES_IN_FLIGHT real frames can be CPU-side prepared at the same
   time. Two is enough to overlap CPU with GPU without queueing extra latency.
   Real swapchain image count comes from the surface (typically 2 or 3). */
/* A5: one CPU-side frame in flight at a time. The fence-wait at the top of
   the main loop pins CPU exactly one frame behind GPU, matching the D3D11
   path's max-frame-latency=1 model. */
#define FRAMES_IN_FLIGHT 1
#define MAX_SWAPCHAIN_IMAGES 8

/* Per-frame-slot CPU/GPU pacing. The fence keeps CPU at most FRAMES_IN_FLIGHT
   ahead of GPU; the command buffer and staging buffer are rotated so we never
   edit one the GPU is still consuming. image_available is also per-frame-slot
   — it's signaled by vkAcquireNextImageKHR and consumed by the next submit,
   both gated by the fence we wait on at the start of each frame, so reuse is
   safe. */
typedef struct {
    VkFence         in_flight;
    VkCommandBuffer cmd;
    VkSemaphore     image_available;

    /* Staging buffer for CPU->GPU pixel upload. Persistently mapped. */
    VkBuffer        staging_buf;
    VkDeviceMemory  staging_mem;
    void           *staging_ptr;
    VkDeviceSize    staging_size;

    /* Descriptor set rebound each frame (one set per frame slot, but they all
       point at the same upload image — kept simple). */
    VkDescriptorSet desc_set;
} FrameSync;

/* render_finished must be per-swapchain-image — see swapchain semaphore reuse
   guide. Framebuffers are also per-swapchain-image. */
typedef struct {
    VkSemaphore   render_finished;
    VkFramebuffer framebuffer;
    VkImageView   view;
} ImageSync;

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

    VkSurfaceKHR   surface;
    VkSwapchainKHR swapchain;
    VkFormat       sc_format;
    VkColorSpaceKHR sc_colorspace;
    VkExtent2D     sc_extent;
    uint32_t       sc_image_count;
    VkImage        sc_images[MAX_SWAPCHAIN_IMAGES];

    VkCommandPool  cmd_pool;
    FrameSync      frames[FRAMES_IN_FLIGHT];
    ImageSync      image_sync[MAX_SWAPCHAIN_IMAGES];
    uint32_t       frame_index;
    int            swapchain_needs_recreate;

    /* Upload image: the emulator's framebuffer copied here each frame.
       Sized to (max_w, max_h); only the (frame_w, frame_h) sub-region is
       valid each frame. UV scale in the push constants masks the rest. */
    VkImage        upload_image;
    VkDeviceMemory upload_mem;
    VkImageView    upload_view;
    VkSampler      sampler;

    /* Render pass / pipeline / descriptors. */
    VkRenderPass        render_pass;
    VkDescriptorSetLayout dset_layout;
    VkDescriptorPool    dset_pool;
    VkPipelineLayout    pipeline_layout;
    VkPipeline          pipeline;
    VkShaderModule      vs_module;
    VkShaderModule      fs_module;

    int upload_image_initialized; /* layout has been set at least once */

    /* Present-mode selection. Resolved at init from env vars. */
    VkPresentModeKHR present_mode;
    int pace_log; /* per-second swapchain/present diagnostics */
    int present_count; /* monotonic, for pace_log */

    /* Swapchain extension PFNs resolved via vkGetDeviceProcAddr so we don't
     * go through the loader trampoline. The trampoline's WSI dispatch can
     * get corrupted on Windows + NVIDIA when a second VkInstance is created
     * for the same physical device, which would otherwise crash present. */
    PFN_vkAcquireNextImageKHR              pfn_AcquireNextImage;
    PFN_vkQueuePresentKHR                  pfn_QueuePresent;

#ifdef ME_HAVE_LSFG
    /* B3: Shared images + timeline semaphore for LSFG frame gen.
     * Two source images (curr/prev), N dest images (generated frames).
     * Memory is exported as Win32 HANDLEs and handed to the backend.
     *
     * We keep one set at the renderer's max_w x max_h resolution.
     * When the core changes resolution we tear down and rebuild.
     *
     * Source images are VK_FORMAT_B8G8R8A8_UNORM (matching upload_image).
     * LSFG expects RGBA8 (or RGBA16F for HDR); we use RGBA8 = VK_FORMAT_R8G8B8A8_UNORM.
     * We blit BGRX -> RGBA via a layout/swizzle copy each frame. */
    int                lsfg_enabled;   /* 1 = backend wired up */
    unsigned           lsfg_width;     /* resolution the context was opened at */
    unsigned           lsfg_height;

    /* Source images (shared with backend via same VkDevice, RGBA8). */
    VkImage            lsfg_src[2];
    VkDeviceMemory     lsfg_src_mem[2];

    /* Dest images (N = multiplier-1; for now 1 dest = 2x). */
    VkImage            lsfg_dst[4];      /* max 4x multiplier */
    VkDeviceMemory     lsfg_dst_mem[4];
    int                lsfg_dst_count;

    /* Timeline semaphore for GPU-GPU sync between our queue and backend
     * (shared-device path: plain VkSemaphore, no Win32 export). */
    VkSemaphore        lsfg_timeline;
    uint64_t           lsfg_timeline_value;

    /* Which source slot we wrote last (0 or 1, alternating). */
    int                lsfg_src_slot;
    int                lsfg_multiplier; /* 2, 3, or 4 */

    /* Backend objects. */
    me_lsfg_instance  *lsfg_inst;
    me_lsfg_context   *lsfg_ctx;

    /* vkWaitSemaphores PFN — from core Vulkan 1.2 promoted from KHR. */
    PFN_vkWaitSemaphores       pfn_WaitSemaphores;
    PFN_vkSignalSemaphore      pfn_SignalSemaphore;

    /* PFN for device-level Win32 external memory/semaphore. */
    PFN_vkGetMemoryWin32HandleKHR          pfn_GetMemoryWin32Handle;
    PFN_vkImportSemaphoreWin32HandleKHR    pfn_ImportSemaphoreWin32Handle;
    PFN_vkGetSemaphoreWin32HandleKHR       pfn_GetSemaphoreWin32Handle;

    /* B4: Command pool + buffer for generated-frame blit+present passes.
     * We reuse a single command buffer (reset each time) — the CPU-wait on
     * the timeline gate ensures the backend is done before we record. */
    VkCommandPool      lsfg_cmd_pool;
    VkCommandBuffer    lsfg_cmd;         /* single cmd buf for generated presents */
    VkSemaphore        lsfg_acquire_sem; /* image_available for generated presents */
    VkFence            lsfg_fence;       /* CPU gate: gpu done with lsfg_cmd */
#endif /* ME_HAVE_LSFG */
} g_vk;

static VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;

/* ------------------------------------------------------------------------- */
/* Helpers                                                                    */
/* ------------------------------------------------------------------------- */

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

static int find_mem_type(uint32_t type_bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & want) == want) {
            return (int)i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Swapchain + framebuffers + per-image views                                 */
/* ------------------------------------------------------------------------- */

/* Resolve which Vulkan present mode to use.
   - LAGGUELESS_VK_NO_VSYNC=1 → IMMEDIATE if supported (tearing, lowest latency).
   - LAGGUELESS_VK_MAILBOX=1  → MAILBOX if supported (no tearing, replaces queued frame).
   - default                  → FIFO (vsync on, always available).
   Returns VK_PRESENT_MODE_FIFO_KHR if the requested mode isn't supported. */
static VkPresentModeKHR resolve_present_mode(VkPresentModeKHR want) {
    if (want == VK_PRESENT_MODE_FIFO_KHR) return VK_PRESENT_MODE_FIFO_KHR;
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &count, NULL);
    if (count == 0) return VK_PRESENT_MODE_FIFO_KHR;
    VkPresentModeKHR *modes = calloc(count, sizeof(*modes));
    if (!modes) return VK_PRESENT_MODE_FIFO_KHR;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &count, modes);
    VkPresentModeKHR result = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < count; i++) {
        if (modes[i] == want) { result = want; break; }
    }
    free(modes);
    return result;
}

static const char *present_mode_str(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE (no vsync)";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO (vsync)";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "?";
    }
}

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

static int create_swapchain_only(void) {
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
        g_vk.swapchain_needs_recreate = 1;
        return 0;
    }

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > MAX_SWAPCHAIN_IMAGES) want = MAX_SWAPCHAIN_IMAGES;

    VkSwapchainKHR old = g_vk.swapchain;

    VkSwapchainCreateInfoKHR sci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_vk.surface,
        .minImageCount = want,
        .imageFormat = g_vk.sc_format,
        .imageColorSpace = g_vk.sc_colorspace,
        .imageExtent = g_vk.sc_extent,
        .imageArrayLayers = 1,
        /* B4: TRANSFER_DST needed when blitting LSFG generated frames into swapchain. */
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = g_vk.present_mode,
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

    printf("[vk] swapchain: %ux%u, %u images, format=%d, %s\n",
           g_vk.sc_extent.width, g_vk.sc_extent.height, g_vk.sc_image_count,
           (int)g_vk.sc_format, present_mode_str(g_vk.present_mode));
    /* IMMEDIATE = the Vulkan equivalent of D3D11's ALLOW_TEARING. The Windows
       VRR pipeline kicks in transparently when adaptive sync is enabled in
       the driver/OS; on a fixed-rate monitor the same setting allows tearing
       for lower latency. Vulkan exposes no portable VRR query on Windows. */
    if (g_vk.present_mode == VK_PRESENT_MODE_IMMEDIATE_KHR) {
        printf("[vk] note: IMMEDIATE mode = VRR if adaptive sync is enabled,\n"
               "[vk]       otherwise tearing for lowest latency on fixed-rate displays.\n");
    }

    g_vk.swapchain_needs_recreate = 0;
    return 0;
}

static void destroy_image_sync(void) {
    for (uint32_t i = 0; i < MAX_SWAPCHAIN_IMAGES; i++) {
        if (g_vk.image_sync[i].framebuffer) {
            vkDestroyFramebuffer(g_vk.device, g_vk.image_sync[i].framebuffer, NULL);
            g_vk.image_sync[i].framebuffer = VK_NULL_HANDLE;
        }
        if (g_vk.image_sync[i].view) {
            vkDestroyImageView(g_vk.device, g_vk.image_sync[i].view, NULL);
            g_vk.image_sync[i].view = VK_NULL_HANDLE;
        }
        if (g_vk.image_sync[i].render_finished) {
            vkDestroySemaphore(g_vk.device, g_vk.image_sync[i].render_finished, NULL);
            g_vk.image_sync[i].render_finished = VK_NULL_HANDLE;
        }
    }
}

static int create_image_sync(void) {
    VkSemaphoreCreateInfo seci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < g_vk.sc_image_count; i++) {
        if (vkCreateSemaphore(g_vk.device, &seci, NULL, &g_vk.image_sync[i].render_finished) != VK_SUCCESS)
            return -1;

        VkImageViewCreateInfo vci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = g_vk.sc_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = g_vk.sc_format,
            .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (vkCreateImageView(g_vk.device, &vci, NULL, &g_vk.image_sync[i].view) != VK_SUCCESS)
            return -1;

        VkFramebufferCreateInfo fci = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = g_vk.render_pass,
            .attachmentCount = 1,
            .pAttachments = &g_vk.image_sync[i].view,
            .width = g_vk.sc_extent.width,
            .height = g_vk.sc_extent.height,
            .layers = 1,
        };
        if (vkCreateFramebuffer(g_vk.device, &fci, NULL, &g_vk.image_sync[i].framebuffer) != VK_SUCCESS)
            return -1;
    }
    return 0;
}

static int recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.device);
    destroy_image_sync();
    if (create_swapchain_only() != 0) return -1;
    if (g_vk.swapchain == VK_NULL_HANDLE) return 0; /* minimized */
    return create_image_sync();
}

/* ------------------------------------------------------------------------- */
/* Upload image, sampler, render pass, pipeline                               */
/* ------------------------------------------------------------------------- */

static int create_upload_image(void) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = { g_vk.max_w, g_vk.max_h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        /* B4: also TRANSFER_SRC so we can blit into LSFG source slots */
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
               | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(g_vk.device, &ici, NULL, &g_vk.upload_image) != VK_SUCCESS) return -1;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.device, g_vk.upload_image, &mr);
    int mt = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return -1;
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(g_vk.device, &mai, NULL, &g_vk.upload_mem) != VK_SUCCESS) return -1;
    if (vkBindImageMemory(g_vk.device, g_vk.upload_image, g_vk.upload_mem, 0) != VK_SUCCESS) return -1;

    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = g_vk.upload_image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (vkCreateImageView(g_vk.device, &vci, NULL, &g_vk.upload_view) != VK_SUCCESS) return -1;

    /* Point sampler, clamp. Matches the integer-scaler intent: no filtering. */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .anisotropyEnable = VK_FALSE,
        .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };
    if (vkCreateSampler(g_vk.device, &sci, NULL, &g_vk.sampler) != VK_SUCCESS) return -1;
    return 0;
}

static int create_render_pass(void) {
    VkAttachmentDescription att = {
        .format = g_vk.sc_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    return vkCreateRenderPass(g_vk.device, &rpci, NULL, &g_vk.render_pass) == VK_SUCCESS ? 0 : -1;
}

static int create_descriptors_and_pipeline(void) {
    /* Descriptor set layout: one combined image sampler at binding 0. */
    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dlci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &b,
    };
    if (vkCreateDescriptorSetLayout(g_vk.device, &dlci, NULL, &g_vk.dset_layout) != VK_SUCCESS) return -1;

    VkDescriptorPoolSize ps = {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = FRAMES_IN_FLIGHT,
    };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = FRAMES_IN_FLIGHT,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    if (vkCreateDescriptorPool(g_vk.device, &dpci, NULL, &g_vk.dset_pool) != VK_SUCCESS) return -1;

    VkDescriptorSetLayout layouts[FRAMES_IN_FLIGHT];
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) layouts[i] = g_vk.dset_layout;
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = g_vk.dset_pool,
        .descriptorSetCount = FRAMES_IN_FLIGHT,
        .pSetLayouts = layouts,
    };
    VkDescriptorSet sets[FRAMES_IN_FLIGHT];
    if (vkAllocateDescriptorSets(g_vk.device, &dsai, sets) != VK_SUCCESS) return -1;
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) g_vk.frames[i].desc_set = sets[i];

    /* Point them all at the upload image. */
    for (int i = 0; i < FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo dii = {
            .sampler = g_vk.sampler,
            .imageView = g_vk.upload_view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };
        VkWriteDescriptorSet w = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = g_vk.frames[i].desc_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &dii,
        };
        vkUpdateDescriptorSets(g_vk.device, 1, &w, 0, NULL);
    }

    /* Pipeline layout: push constants for rect + uvscale (32 bytes). */
    VkPushConstantRange pcr = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = 32,
    };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &g_vk.dset_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if (vkCreatePipelineLayout(g_vk.device, &plci, NULL, &g_vk.pipeline_layout) != VK_SUCCESS) return -1;

    /* Shader modules. */
    VkShaderModuleCreateInfo vsmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = quad_vert_spv_len,
        .pCode = quad_vert_spv,
    };
    if (vkCreateShaderModule(g_vk.device, &vsmci, NULL, &g_vk.vs_module) != VK_SUCCESS) return -1;
    VkShaderModuleCreateInfo fsmci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = quad_frag_spv_len,
        .pCode = quad_frag_spv,
    };
    if (vkCreateShaderModule(g_vk.device, &fsmci, NULL, &g_vk.fs_module) != VK_SUCCESS) return -1;

    /* Pipeline. */
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_vk.vs_module, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_vk.fs_module, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .blendEnable = VK_FALSE,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                        | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    VkDynamicState dyns[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyns,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .pDynamicState = &dy,
        .layout = g_vk.pipeline_layout,
        .renderPass = g_vk.render_pass,
        .subpass = 0,
    };
    if (vkCreateGraphicsPipelines(g_vk.device, VK_NULL_HANDLE, 1, &gpci, NULL, &g_vk.pipeline) != VK_SUCCESS) return -1;
    return 0;
}

static int create_frame_resources(void) {
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_vk.gfx_queue_family,
    };
    if (vkCreateCommandPool(g_vk.device, &cpci, NULL, &g_vk.cmd_pool) != VK_SUCCESS) return -1;

    VkSemaphoreCreateInfo seci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT,
    };
    VkDeviceSize staging_size = (VkDeviceSize)g_vk.max_w * g_vk.max_h * 4;
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(g_vk.device, &seci, NULL, &g_vk.frames[i].image_available) != VK_SUCCESS) return -1;
        if (vkCreateFence(g_vk.device, &fci, NULL, &g_vk.frames[i].in_flight) != VK_SUCCESS) return -1;
        VkCommandBufferAllocateInfo cbi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g_vk.cmd_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (vkAllocateCommandBuffers(g_vk.device, &cbi, &g_vk.frames[i].cmd) != VK_SUCCESS) return -1;

        /* Staging buffer: persistently mapped host-visible memory. */
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = staging_size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        if (vkCreateBuffer(g_vk.device, &bci, NULL, &g_vk.frames[i].staging_buf) != VK_SUCCESS) return -1;

        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g_vk.device, g_vk.frames[i].staging_buf, &mr);
        int mt = find_mem_type(mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (mt < 0) return -1;
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = mr.size,
            .memoryTypeIndex = (uint32_t)mt,
        };
        if (vkAllocateMemory(g_vk.device, &mai, NULL, &g_vk.frames[i].staging_mem) != VK_SUCCESS) return -1;
        if (vkBindBufferMemory(g_vk.device, g_vk.frames[i].staging_buf, g_vk.frames[i].staging_mem, 0) != VK_SUCCESS) return -1;
        if (vkMapMemory(g_vk.device, g_vk.frames[i].staging_mem, 0, VK_WHOLE_SIZE, 0, &g_vk.frames[i].staging_ptr) != VK_SUCCESS) return -1;
        g_vk.frames[i].staging_size = staging_size;
    }
    return 0;
}

static void destroy_frame_resources(void) {
    for (uint32_t i = 0; i < FRAMES_IN_FLIGHT; i++) {
        if (g_vk.frames[i].staging_ptr) {
            vkUnmapMemory(g_vk.device, g_vk.frames[i].staging_mem);
            g_vk.frames[i].staging_ptr = NULL;
        }
        if (g_vk.frames[i].staging_buf) {
            vkDestroyBuffer(g_vk.device, g_vk.frames[i].staging_buf, NULL);
            g_vk.frames[i].staging_buf = VK_NULL_HANDLE;
        }
        if (g_vk.frames[i].staging_mem) {
            vkFreeMemory(g_vk.device, g_vk.frames[i].staging_mem, NULL);
            g_vk.frames[i].staging_mem = VK_NULL_HANDLE;
        }
        if (g_vk.frames[i].image_available) vkDestroySemaphore(g_vk.device, g_vk.frames[i].image_available, NULL);
        if (g_vk.frames[i].in_flight) vkDestroyFence(g_vk.device, g_vk.frames[i].in_flight, NULL);
        memset(&g_vk.frames[i], 0, sizeof(g_vk.frames[i]));
    }
    if (g_vk.cmd_pool) {
        vkDestroyCommandPool(g_vk.device, g_vk.cmd_pool, NULL);
        g_vk.cmd_pool = VK_NULL_HANDLE;
    }
}

/* ------------------------------------------------------------------------- */
/* init / shutdown                                                            */
/* ------------------------------------------------------------------------- */

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

    VkWin32SurfaceCreateInfoKHR wci = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hinstance = GetModuleHandleW(NULL),
        .hwnd = hwnd,
    };
    VK_CHECK(vkCreateWin32SurfaceKHR(g_vk.instance, &wci, NULL, &g_vk.surface), "vkCreateWin32SurfaceKHR");

    uint32_t pcount = 0;
    VK_CHECK(vkEnumeratePhysicalDevices(g_vk.instance, &pcount, NULL), "vkEnumeratePhysicalDevices(count)");
    if (pcount == 0) {
        fprintf(stderr, "[vk] no Vulkan physical devices found\n");
        goto fail;
    }
    VkPhysicalDevice *phys = calloc(pcount, sizeof(*phys));
    if (!phys) goto fail;
    if (vkEnumeratePhysicalDevices(g_vk.instance, &pcount, phys) != VK_SUCCESS) { free(phys); goto fail; }

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

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = g_vk.gfx_queue_family,
        .queueCount = 1,
        .pQueuePriorities = &qprio,
    };
    const char *dev_exts[8];
    uint32_t dev_ext_count = 0;
    dev_exts[dev_ext_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#ifdef ME_HAVE_LSFG
    /* B3: External memory/semaphore extensions needed for HANDLE sharing.
     * Timeline semaphore extension is enabled in addition to the 1.2 feature
     * because the shared lsfg-vk backend resolves the KHR-suffixed entry points
     * (vkSignalSemaphoreKHR / vkWaitSemaphoresKHR). */
    dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
    dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;
    dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
    dev_exts[dev_ext_count++] = VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME;
    dev_exts[dev_ext_count++] = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
#endif
    /* Enable timeline semaphores (needed for vkWaitSemaphores on our device)
       and explicitly disable samplerAnisotropy (we don't use it; prevents
       validation errors from other internal paths that may set VK_TRUE). */
    VkPhysicalDeviceFeatures feats = { .samplerAnisotropy = VK_FALSE };
    VkPhysicalDeviceVulkan12Features feats12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore = VK_TRUE,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = &feats12,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
        .enabledExtensionCount = dev_ext_count,
        .ppEnabledExtensionNames = dev_exts,
        .pEnabledFeatures = &feats,
    };
    VK_CHECK(vkCreateDevice(g_vk.phys, &dci, NULL, &g_vk.device), "vkCreateDevice");
    vkGetDeviceQueue(g_vk.device, g_vk.gfx_queue_family, 0, &g_vk.gfx_queue);

    /* Resolve swapchain extension PFNs from the device. Calling these through
     * the loader trampoline can crash after a second VkInstance is created
     * (the lsfg-vk backend). Calling the device-level pointer directly is safe
     * because it points straight at the ICD entry point. */
    g_vk.pfn_AcquireNextImage = (PFN_vkAcquireNextImageKHR)
        vkGetDeviceProcAddr(g_vk.device, "vkAcquireNextImageKHR");
    g_vk.pfn_QueuePresent = (PFN_vkQueuePresentKHR)
        vkGetDeviceProcAddr(g_vk.device, "vkQueuePresentKHR");
    if (!g_vk.pfn_AcquireNextImage || !g_vk.pfn_QueuePresent) {
        fprintf(stderr, "[vk] failed to resolve swapchain device PFNs\n");
        goto fail;
    }

#ifdef ME_HAVE_LSFG
    /* Load Vulkan 1.2 timeline semaphore PFNs (CPU-side wait/signal). */
    g_vk.pfn_WaitSemaphores = (PFN_vkWaitSemaphores)
        vkGetDeviceProcAddr(g_vk.device, "vkWaitSemaphores");
    g_vk.pfn_SignalSemaphore = (PFN_vkSignalSemaphore)
        vkGetDeviceProcAddr(g_vk.device, "vkSignalSemaphore");
    /* Load Win32 external memory/semaphore device functions. */
    g_vk.pfn_GetMemoryWin32Handle = (PFN_vkGetMemoryWin32HandleKHR)
        vkGetDeviceProcAddr(g_vk.device, "vkGetMemoryWin32HandleKHR");
    g_vk.pfn_ImportSemaphoreWin32Handle = (PFN_vkImportSemaphoreWin32HandleKHR)
        vkGetDeviceProcAddr(g_vk.device, "vkImportSemaphoreWin32HandleKHR");
    g_vk.pfn_GetSemaphoreWin32Handle = (PFN_vkGetSemaphoreWin32HandleKHR)
        vkGetDeviceProcAddr(g_vk.device, "vkGetSemaphoreWin32HandleKHR");
    if (!g_vk.pfn_WaitSemaphores) {
        fprintf(stderr, "[vk] WARNING: vkWaitSemaphores unavailable - LSFG disabled\n");
    }
    /* pfn_GetMemoryWin32Handle/etc are no longer required (shared-device path)
     * but we keep loading them above; unused PFN slots remain harmless. */
#endif

    /* Resolve present mode. Defaults to FIFO; opt-in to MAILBOX or IMMEDIATE
       via env vars. Per-frame pacing (frames-in-flight=1) does the bulk of
       latency reduction regardless. */
    {
        const char *no_vsync = getenv("LAGGUELESS_VK_NO_VSYNC");
        const char *mailbox  = getenv("LAGGUELESS_VK_MAILBOX");
        VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
        if (no_vsync && no_vsync[0] && no_vsync[0] != '0') want = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (mailbox && mailbox[0] && mailbox[0] != '0') want = VK_PRESENT_MODE_MAILBOX_KHR;
        g_vk.present_mode = resolve_present_mode(want);
        if (g_vk.present_mode != want) {
            fprintf(stderr, "[vk] requested present mode %s not supported, using %s\n",
                    present_mode_str(want), present_mode_str(g_vk.present_mode));
        }
    }
    {
        const char *pl = getenv("LAGGUELESS_VK_PACE_LOG");
        g_vk.pace_log = (pl && pl[0] && pl[0] != '0');
    }

    /* The render pass needs sc_format; create the swapchain first (without
       framebuffers/views), then the render pass, then the views/framebuffers. */
    if (create_swapchain_only() != 0) goto fail;
    if (create_render_pass() != 0)    goto fail;
    if (create_upload_image() != 0)   goto fail;
    if (create_descriptors_and_pipeline() != 0) goto fail;
    if (create_frame_resources() != 0) goto fail;
    if (g_vk.swapchain != VK_NULL_HANDLE && create_image_sync() != 0) goto fail;

    g_vk.active = 1;
    return 0;

fail:
    me_vk_shutdown();
    return -1;
}

/* -------------------------------------------------------------------------
 * B3 LSFG shared-image helpers (compiled only with ME_HAVE_LSFG)
 * ------------------------------------------------------------------------- */
#ifdef ME_HAVE_LSFG

/* Create a plain RGBA8 image for LSFG source/dest slots (shared-device path).
 * No Win32 export — host and backend share the same VkDevice. */
static int create_lsfg_image(unsigned w, unsigned h,
                             VkImage *out_img, VkDeviceMemory *out_mem) {
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM, /* LSFG expects RGBA */
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | /* needed to blit OUT to swapchain */
                 VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (vkCreateImage(g_vk.device, &ici, NULL, out_img) != VK_SUCCESS) return -1;

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.device, *out_img, &mr);

    int mt = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return -1;
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = (uint32_t)mt,
    };
    if (vkAllocateMemory(g_vk.device, &mai, NULL, out_mem) != VK_SUCCESS) return -1;
    if (vkBindImageMemory(g_vk.device, *out_img, *out_mem, 0) != VK_SUCCESS) return -1;
    return 0;
}

/* Create a plain timeline semaphore (shared-device path, no Win32 export). */
static int create_lsfg_timeline(VkSemaphore *out_sem, uint64_t initial_value) {
    VkSemaphoreTypeCreateInfo stci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
        .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
        .initialValue = initial_value,
    };
    VkSemaphoreCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
        .pNext = &stci,
    };
    if (vkCreateSemaphore(g_vk.device, &sci, NULL, out_sem) != VK_SUCCESS) return -1;
    return 0;
}

static void destroy_lsfg_images(void) {
    /* B4: Destroy command infrastructure first. */
    if (g_vk.lsfg_fence)       { vkDestroyFence(g_vk.device, g_vk.lsfg_fence, NULL);           g_vk.lsfg_fence = VK_NULL_HANDLE; }
    if (g_vk.lsfg_acquire_sem) { vkDestroySemaphore(g_vk.device, g_vk.lsfg_acquire_sem, NULL); g_vk.lsfg_acquire_sem = VK_NULL_HANDLE; }
    if (g_vk.lsfg_cmd_pool)    { vkDestroyCommandPool(g_vk.device, g_vk.lsfg_cmd_pool, NULL);  g_vk.lsfg_cmd_pool = VK_NULL_HANDLE; }
    g_vk.lsfg_cmd = VK_NULL_HANDLE;

    /* Destroy Vulkan objects (shared-device path: host owns everything). */
    for (int i = 0; i < 2; i++) {
        if (g_vk.lsfg_src[i])        { vkDestroyImage(g_vk.device, g_vk.lsfg_src[i], NULL); g_vk.lsfg_src[i] = VK_NULL_HANDLE; }
        if (g_vk.lsfg_src_mem[i])    { vkFreeMemory(g_vk.device, g_vk.lsfg_src_mem[i], NULL); g_vk.lsfg_src_mem[i] = VK_NULL_HANDLE; }
    }
    for (int i = 0; i < g_vk.lsfg_dst_count; i++) {
        if (g_vk.lsfg_dst[i])        { vkDestroyImage(g_vk.device, g_vk.lsfg_dst[i], NULL); g_vk.lsfg_dst[i] = VK_NULL_HANDLE; }
        if (g_vk.lsfg_dst_mem[i])    { vkFreeMemory(g_vk.device, g_vk.lsfg_dst_mem[i], NULL); g_vk.lsfg_dst_mem[i] = VK_NULL_HANDLE; }
    }
    g_vk.lsfg_dst_count = 0;
    if (g_vk.lsfg_timeline)        { vkDestroySemaphore(g_vk.device, g_vk.lsfg_timeline, NULL); g_vk.lsfg_timeline = VK_NULL_HANDLE; }
    g_vk.lsfg_timeline_value = 0;
}

/* -------------------------------------------------------------------------
 * me_vk_lsfg_init — B4 public API
 * Call after me_vk_init() to wire up the LSFG backend and force FIFO mode.
 * ------------------------------------------------------------------------- */
int me_vk_lsfg_init(const char *dll_path, unsigned width, unsigned height, int multiplier) {
    if (!g_vk.active) return -1;
    if (!g_vk.pfn_WaitSemaphores) {
        fprintf(stderr, "[vk-lsfg] vkWaitSemaphores PFN missing - LSFG disabled\n");
        return -1;
    }
    if (multiplier < 2) multiplier = 2;
    if (multiplier > 4) multiplier = 4;
    int dst_count = multiplier - 1;

    /* B4: Switch to FIFO — required for proper pacing with frame gen. */
    if (g_vk.present_mode != VK_PRESENT_MODE_FIFO_KHR) {
        fprintf(stderr, "[vk-lsfg] switching to FIFO present mode for frame gen\n");
        g_vk.present_mode = VK_PRESENT_MODE_FIFO_KHR;
        /* Recreate immediately so the first present hits a steady state. */
        if (recreate_swapchain() != 0) {
            fprintf(stderr, "[vk-lsfg] failed to recreate swapchain in FIFO mode\n");
            goto fail;
        }
    }

    /* B4: Create command pool + buffer for generated-frame presents. */
    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = g_vk.gfx_queue_family,
    };
    if (vkCreateCommandPool(g_vk.device, &cpci, NULL, &g_vk.lsfg_cmd_pool) != VK_SUCCESS) {
        fprintf(stderr, "[vk-lsfg] failed to create B4 command pool\n");
        goto fail;
    }
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g_vk.lsfg_cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    if (vkAllocateCommandBuffers(g_vk.device, &cbai, &g_vk.lsfg_cmd) != VK_SUCCESS) {
        fprintf(stderr, "[vk-lsfg] failed to allocate B4 command buffer\n");
        goto fail;
    }
    VkFenceCreateInfo fci = {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .flags = VK_FENCE_CREATE_SIGNALED_BIT, /* start signaled so first wait is free */
    };
    if (vkCreateFence(g_vk.device, &fci, NULL, &g_vk.lsfg_fence) != VK_SUCCESS) {
        fprintf(stderr, "[vk-lsfg] failed to create B4 fence\n");
        goto fail;
    }
    VkSemaphoreCreateInfo seci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(g_vk.device, &seci, NULL, &g_vk.lsfg_acquire_sem) != VK_SUCCESS) {
        fprintf(stderr, "[vk-lsfg] failed to create B4 acquire semaphore\n");
        goto fail;
    }

    /* Create the backend instance. It shares our VkInstance/VkDevice rather
     * than creating its own — on Windows + NVIDIA, creating a second VkInstance
     * for the same physical device clobbers the ICD's WSI dispatch and the
     * host's vkQueuePresentKHR crashes (jump to 0x0). */
    g_vk.lsfg_inst = me_lsfg_backend_create(
        dll_path, g_vk.instance, g_vk.phys, g_vk.device,
        g_vk.gfx_queue, g_vk.gfx_queue_family);
    if (!g_vk.lsfg_inst) goto fail;

    /* Create shared source images (no Win32 export — host/backend share VkDevice). */
    for (int i = 0; i < 2; i++) {
        if (create_lsfg_image(width, height,
                              &g_vk.lsfg_src[i], &g_vk.lsfg_src_mem[i]) != 0) {
            fprintf(stderr, "[vk-lsfg] failed to create source image %d\n", i);
            goto fail;
        }
    }

    /* Create shared dest images. */
    for (int i = 0; i < dst_count; i++) {
        if (create_lsfg_image(width, height,
                              &g_vk.lsfg_dst[i], &g_vk.lsfg_dst_mem[i]) != 0) {
            fprintf(stderr, "[vk-lsfg] failed to create dest image %d\n", i);
            goto fail;
        }
    }
    g_vk.lsfg_dst_count = dst_count;

    /* Create timeline semaphore. */
    if (create_lsfg_timeline(&g_vk.lsfg_timeline, 0) != 0) {
        fprintf(stderr, "[vk-lsfg] failed to create timeline semaphore\n");
        goto fail;
    }

    /* Open the backend context via the shared-device path. */
    g_vk.lsfg_ctx = me_lsfg_backend_open_shared(
        g_vk.lsfg_inst,
        g_vk.lsfg_src[0], g_vk.lsfg_src[1],
        g_vk.lsfg_src_mem[0], g_vk.lsfg_src_mem[1],
        g_vk.lsfg_dst, g_vk.lsfg_dst_mem, dst_count,
        g_vk.lsfg_timeline,
        width, height,
        0 /* hdr=false */, 1.0f /* flow_scale */, 0 /* perf_mode */
    );
    if (!g_vk.lsfg_ctx) {
        fprintf(stderr, "[vk-lsfg] backend openContext failed\n");
        goto fail;
    }

    g_vk.lsfg_enabled     = 1;
    g_vk.lsfg_width       = width;
    g_vk.lsfg_height      = height;
    g_vk.lsfg_src_slot    = 0;
    g_vk.lsfg_multiplier  = multiplier;
    g_vk.lsfg_timeline_value = 0;
    fprintf(stderr, "[vk-lsfg] B4 OK: frame gen ready (%ux%u, x%d, FIFO)\n",
            width, height, multiplier);
    return 0;

fail:
    if (g_vk.lsfg_ctx) {
        me_lsfg_backend_close(g_vk.lsfg_inst, g_vk.lsfg_ctx);
        g_vk.lsfg_ctx = NULL;
    }
    destroy_lsfg_images();
    if (g_vk.lsfg_inst) {
        me_lsfg_backend_destroy(g_vk.lsfg_inst);
        g_vk.lsfg_inst = NULL;
    }
    return -1;
}

void me_vk_lsfg_shutdown(void) {
    if (!g_vk.lsfg_inst) return;
    if (g_vk.device) vkDeviceWaitIdle(g_vk.device);
    if (g_vk.lsfg_ctx) {
        me_lsfg_backend_close(g_vk.lsfg_inst, g_vk.lsfg_ctx);
        g_vk.lsfg_ctx = NULL;
    }
    destroy_lsfg_images();
    me_lsfg_backend_destroy(g_vk.lsfg_inst);
    g_vk.lsfg_inst = NULL;
    g_vk.lsfg_enabled = 0;
}

#endif /* ME_HAVE_LSFG */


void me_vk_shutdown(void) {
    if (g_vk.device) {
        vkDeviceWaitIdle(g_vk.device);
    }
    destroy_frame_resources();
    destroy_image_sync();

    if (g_vk.pipeline)        vkDestroyPipeline(g_vk.device, g_vk.pipeline, NULL);
    if (g_vk.pipeline_layout) vkDestroyPipelineLayout(g_vk.device, g_vk.pipeline_layout, NULL);
    if (g_vk.vs_module)       vkDestroyShaderModule(g_vk.device, g_vk.vs_module, NULL);
    if (g_vk.fs_module)       vkDestroyShaderModule(g_vk.device, g_vk.fs_module, NULL);
    if (g_vk.dset_pool)       vkDestroyDescriptorPool(g_vk.device, g_vk.dset_pool, NULL);
    if (g_vk.dset_layout)     vkDestroyDescriptorSetLayout(g_vk.device, g_vk.dset_layout, NULL);
    if (g_vk.render_pass)     vkDestroyRenderPass(g_vk.device, g_vk.render_pass, NULL);

    if (g_vk.sampler)         vkDestroySampler(g_vk.device, g_vk.sampler, NULL);
    if (g_vk.upload_view)     vkDestroyImageView(g_vk.device, g_vk.upload_view, NULL);
    if (g_vk.upload_image)    vkDestroyImage(g_vk.device, g_vk.upload_image, NULL);
    if (g_vk.upload_mem)      vkFreeMemory(g_vk.device, g_vk.upload_mem, NULL);

    if (g_vk.swapchain)       vkDestroySwapchainKHR(g_vk.device, g_vk.swapchain, NULL);

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
    memset(&g_vk, 0, sizeof(g_vk));
}

int me_vk_is_active(void) { return g_vk.active; }

int me_vk_wait_for_present(unsigned timeout_ms) {
    if (!g_vk.active) return 0;
    /* With FRAMES_IN_FLIGHT=1 the single fence is the "previous frame is
       done" signal. Matches the DXGI waitable-swap-chain semantics. */
    VkFence f = g_vk.frames[0].in_flight;
    if (f == VK_NULL_HANDLE) return 0;
    uint64_t ns = (uint64_t)timeout_ms * 1000000ull;
    vkWaitForFences(g_vk.device, 1, &f, VK_TRUE, ns);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Per-frame upload + draw + present                                          */
/* ------------------------------------------------------------------------- */

int me_vk_present(const u32 *pixels, unsigned frame_w, unsigned frame_h, unsigned max_w,
                  int cw, int ch, int dx, int dy, int dw, int dh) {
    (void)cw; (void)ch; /* viewport is the entire swapchain, scissor handles bars */
    if (!g_vk.active) return -1;

    if (g_vk.swapchain_needs_recreate || g_vk.swapchain == VK_NULL_HANDLE) {
        if (recreate_swapchain() != 0) return -1;
        if (g_vk.swapchain == VK_NULL_HANDLE) return 0; /* minimized */
    }

    FrameSync *f = &g_vk.frames[g_vk.frame_index];

    vkWaitForFences(g_vk.device, 1, &f->in_flight, VK_TRUE, UINT64_MAX);

    uint32_t img_idx = 0;
    VkResult r = g_vk.pfn_AcquireNextImage(g_vk.device, g_vk.swapchain, UINT64_MAX,
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

    /* Clamp uploaded sub-region to the upload image extent. */
    if (frame_w > g_vk.max_w) frame_w = g_vk.max_w;
    if (frame_h > g_vk.max_h) frame_h = g_vk.max_h;

    /* Pack pixels into staging tightly (row by row). The core's backbuffer
       has stride max_w but we only need the (frame_w, frame_h) sub-region. */
    if (pixels && frame_w > 0 && frame_h > 0) {
        uint8_t *dst = (uint8_t*)f->staging_ptr;
        const uint8_t *src = (const uint8_t*)pixels;
        size_t src_stride = (size_t)max_w * 4;
        size_t row_bytes = (size_t)frame_w * 4;
        for (unsigned y = 0; y < frame_h; y++) {
            memcpy(dst + (size_t)y * row_bytes, src + (size_t)y * src_stride, row_bytes);
        }
    }

    /* Build commands. */
    vkResetCommandBuffer(f->cmd, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    if (vkBeginCommandBuffer(f->cmd, &bi) != VK_SUCCESS) return -1;

    /* Transition upload image to TRANSFER_DST (initial UNDEFINED first time,
       SHADER_READ_ONLY thereafter). */
    {
        VkImageMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = g_vk.upload_image_initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = g_vk.upload_image_initialized
                           ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                           : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.upload_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(f->cmd,
            g_vk.upload_image_initialized ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    }

    if (pixels && frame_w > 0 && frame_h > 0) {
        VkBufferImageCopy region = {
            .bufferOffset = 0,
            .bufferRowLength = frame_w,    /* tightly packed */
            .bufferImageHeight = frame_h,
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { 0, 0, 0 },
            .imageExtent = { frame_w, frame_h, 1 },
        };
        vkCmdCopyBufferToImage(f->cmd, f->staging_buf, g_vk.upload_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }

    /* Transition upload image to SHADER_READ_ONLY for sampling. */
    {
        VkImageMemoryBarrier b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.upload_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(f->cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &b);
    }
    g_vk.upload_image_initialized = 1;

#ifdef ME_HAVE_LSFG
    /* B3: Copy upload image into the current LSFG source slot.
     * We blit BGRX (upload, B8G8R8A8_UNORM) -> RGBA (src slot, R8G8B8A8_UNORM).
     * vkCmdBlitImage with NEAREST filter handles the channel reorder via the
     * surface format difference (Vulkan blits swizzle automatically when the
     * component order differs between same-bitwidth formats on most hardware).
     * The upload image is frame_w x frame_h (a sub-region); the source slot is
     * lsfg_width x lsfg_height (== max_w x max_h at context-open time).
     * We blit only the (frame_w, frame_h) region into the source slot. */
    if (g_vk.lsfg_enabled && pixels && frame_w > 0 && frame_h > 0) {
        int slot = g_vk.lsfg_src_slot;

        /* 1. Transition src slot: UNDEFINED -> TRANSFER_DST */
        VkImageMemoryBarrier lsfg_b = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.lsfg_src[slot],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(f->cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &lsfg_b);

        /* 2. Blit upload_image -> lsfg_src[slot].
         * upload_image is currently in SHADER_READ_ONLY; we need TRANSFER_SRC. */
        VkImageMemoryBarrier upload_to_src = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.upload_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(f->cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, NULL, 0, NULL, 1, &upload_to_src);

        VkImageBlit blit_region = {
            .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .srcOffsets = { {0,0,0}, {(int32_t)frame_w, (int32_t)frame_h, 1} },
            .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .dstOffsets = { {0,0,0}, {(int32_t)frame_w, (int32_t)frame_h, 1} },
        };
        vkCmdBlitImage(f->cmd,
            g_vk.upload_image,      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            g_vk.lsfg_src[slot],    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit_region, VK_FILTER_NEAREST);

        /* 3. Restore upload_image -> SHADER_READ_ONLY for the render pass. */
        VkImageMemoryBarrier upload_back = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.upload_image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(f->cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, NULL, 0, NULL, 1, &upload_back);

        /* 4. Transition lsfg_src[slot] -> GENERAL (backend reads in GENERAL). */
        VkImageMemoryBarrier lsfg_to_general = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = g_vk.lsfg_src[slot],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(f->cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, NULL, 0, NULL, 1, &lsfg_to_general);

        /* Advance source slot for next frame. */
        g_vk.lsfg_src_slot = 1 - slot;
    }
#endif /* ME_HAVE_LSFG */


    VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = g_vk.render_pass,
        .framebuffer = g_vk.image_sync[img_idx].framebuffer,
        .renderArea = { { 0, 0 }, g_vk.sc_extent },
        .clearValueCount = 1,
        .pClearValues = &clear,
    };
    vkCmdBeginRenderPass(f->cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = {
        .x = 0, .y = 0,
        .width = (float)g_vk.sc_extent.width,
        .height = (float)g_vk.sc_extent.height,
        .minDepth = 0, .maxDepth = 1,
    };
    VkRect2D sc = { { 0, 0 }, g_vk.sc_extent };
    vkCmdSetViewport(f->cmd, 0, 1, &vp);
    vkCmdSetScissor(f->cmd, 0, 1, &sc);

    vkCmdBindPipeline(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_vk.pipeline);
    vkCmdBindDescriptorSets(f->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            g_vk.pipeline_layout, 0, 1, &f->desc_set, 0, NULL);

    /* Push constants: NDC rect of the integer-scaled dst and the UV scale to
       sample only the active (frame_w, frame_h) sub-region of the upload
       image (sized max_w × max_h). */
    float sw = (float)g_vk.sc_extent.width;
    float sh = (float)g_vk.sc_extent.height;
    float nx0 = (float)dx / sw * 2.0f - 1.0f;
    float ny0 = (float)dy / sh * 2.0f - 1.0f;
    float nx1 = (float)(dx + dw) / sw * 2.0f - 1.0f;
    float ny1 = (float)(dy + dh) / sh * 2.0f - 1.0f;
    float pc[8] = {
        nx0, ny0, nx1, ny1,
        (float)frame_w / (float)g_vk.max_w,
        (float)frame_h / (float)g_vk.max_h,
        0.0f, 0.0f,
    };
    vkCmdPushConstants(f->cmd, g_vk.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT,
                       0, sizeof(pc), pc);
    vkCmdDraw(f->cmd, 4, 1, 0, 0);

    vkCmdEndRenderPass(f->cmd);
    if (vkEndCommandBuffer(f->cmd) != VK_SUCCESS) return -1;

    VkSemaphore signal_sem = g_vk.image_sync[img_idx].render_finished;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

#ifdef ME_HAVE_LSFG
    /* B4: Include the timeline semaphore in the submit signal list.
     * Incrementing by 1 tells the backend "source slot is ready, generate now". */
    if (g_vk.lsfg_enabled && g_vk.lsfg_ctx) {
        g_vk.lsfg_timeline_value++;
        uint64_t signal_val = g_vk.lsfg_timeline_value;
        VkSemaphore signal_sems[2] = { signal_sem, g_vk.lsfg_timeline };
        uint64_t    signal_vals[2] = { 0, signal_val }; /* binary sem uses 0 */
        uint64_t    wait_vals[1]   = { 0 }; /* binary wait sem uses 0 */
        VkTimelineSemaphoreSubmitInfo tsi = {
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = wait_vals,
            .signalSemaphoreValueCount = 2,
            .pSignalSemaphoreValues = signal_vals,
        };
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = &tsi,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &f->image_available,
            .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &f->cmd,
            .signalSemaphoreCount = 2,
            .pSignalSemaphores = signal_sems,
        };
        if (vkQueueSubmit(g_vk.gfx_queue, 1, &si, f->in_flight) != VK_SUCCESS) {
            fprintf(stderr, "[vk] vkQueueSubmit failed\n");
            return -1;
        }

        /* B4: Tell backend to generate frames (it waits on our timeline signal).
         * The backend's GPU submit signals timeline values signal_val+1 .. signal_val+dst_count.
         * Once scheduled, those values are spoken for even if we bail out below — account for
         * them up front so the next iteration's increment doesn't collide with a value the
         * backend will eventually signal (causes VUID-VkSubmitInfo-pSignalSemaphores-03242
         * "signal value must be greater than current"). */
        me_lsfg_backend_schedule(g_vk.lsfg_inst, g_vk.lsfg_ctx);
        g_vk.lsfg_timeline_value += (uint64_t)g_vk.lsfg_dst_count;

        /* Present the real frame first (FIFO: it queues behind any pending present). */
        VkPresentInfoKHR pi = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &signal_sem,
            .swapchainCount = 1,
            .pSwapchains = &g_vk.swapchain,
            .pImageIndices = &img_idx,
        };
        r = g_vk.pfn_QueuePresent(g_vk.gfx_queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            g_vk.swapchain_needs_recreate = 1;
            goto lsfg_present_done;
        } else if (r != VK_SUCCESS) {
            fprintf(stderr, "[vk] vkQueuePresentKHR (real): %s\n", vk_result_str(r));
        }

        /* B4: Wait for backend to finish each generated frame, then blit + present. */
        for (int gi = 0; gi < g_vk.lsfg_dst_count; gi++) {
            /* CPU-wait for this generated frame to be ready on the backend device. */
            uint64_t wait_val = signal_val + (uint64_t)(gi + 1);
            VkSemaphoreWaitInfo swi = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &g_vk.lsfg_timeline,
                .pValues = &wait_val,
            };
            g_vk.pfn_WaitSemaphores(g_vk.device, &swi, 5000000000ull /* 5s timeout */);

            /* Wait for previous lsfg_cmd to finish (first time fence is already signaled). */
            vkWaitForFences(g_vk.device, 1, &g_vk.lsfg_fence, VK_TRUE, UINT64_MAX);
            vkResetFences(g_vk.device, 1, &g_vk.lsfg_fence);

            /* Acquire a swapchain image for this generated frame. */
            uint32_t gen_img_idx = 0;
            r = g_vk.pfn_AcquireNextImage(g_vk.device, g_vk.swapchain, UINT64_MAX,
                                      g_vk.lsfg_acquire_sem, VK_NULL_HANDLE, &gen_img_idx);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) {
                g_vk.swapchain_needs_recreate = 1;
                goto lsfg_present_done;
            }
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                fprintf(stderr, "[vk] gen-frame acquire failed: %s\n", vk_result_str(r));
                goto lsfg_present_done;
            }

            /* Record: blit lsfg_dst[gi] (RGBA8, GENERAL) -> swapchain[gen_img_idx] (BGRA8). */
            vkResetCommandBuffer(g_vk.lsfg_cmd, 0);
            VkCommandBufferBeginInfo lbi = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            };
            vkBeginCommandBuffer(g_vk.lsfg_cmd, &lbi);

            /* Transition swapchain image: UNDEFINED -> TRANSFER_DST */
            VkImageMemoryBarrier sc_to_dst = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = g_vk.sc_images[gen_img_idx],
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            vkCmdPipelineBarrier(g_vk.lsfg_cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, NULL, 0, NULL, 1, &sc_to_dst);

            /* Blit: dest image is in GENERAL (backend leaves it there).
             * Source extent MUST be LSFG resolution (the dst image was created
             * at lsfg_width x lsfg_height). Reading at swapchain extent caused
             * over-reads and visible smearing. Destination spans the full
             * swapchain — aspect/integer scaling is a future refinement. */
            VkImageBlit gen_blit = {
                .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .srcOffsets = { {0,0,0}, {(int32_t)g_vk.lsfg_width,
                                          (int32_t)g_vk.lsfg_height, 1} },
                .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                .dstOffsets = { {0,0,0}, {(int32_t)g_vk.sc_extent.width,
                                          (int32_t)g_vk.sc_extent.height, 1} },
            };
            vkCmdBlitImage(g_vk.lsfg_cmd,
                g_vk.lsfg_dst[gi], VK_IMAGE_LAYOUT_GENERAL,
                g_vk.sc_images[gen_img_idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &gen_blit, VK_FILTER_NEAREST);

            /* Transition swapchain: TRANSFER_DST -> PRESENT_SRC */
            VkImageMemoryBarrier sc_to_present = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = 0,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = g_vk.sc_images[gen_img_idx],
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            vkCmdPipelineBarrier(g_vk.lsfg_cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                0, 0, NULL, 0, NULL, 1, &sc_to_present);

            vkEndCommandBuffer(g_vk.lsfg_cmd);

            VkSemaphore gen_signal = g_vk.image_sync[gen_img_idx].render_finished;
            VkPipelineStageFlags gen_wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo gen_si = {
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &g_vk.lsfg_acquire_sem,
                .pWaitDstStageMask = &gen_wait_stage,
                .commandBufferCount = 1,
                .pCommandBuffers = &g_vk.lsfg_cmd,
                .signalSemaphoreCount = 1,
                .pSignalSemaphores = &gen_signal,
            };
            VkResult gen_sr = vkQueueSubmit(g_vk.gfx_queue, 1, &gen_si, g_vk.lsfg_fence);
            if (gen_sr != VK_SUCCESS) {
                /* Submit failed — the acquired image is still in UNDEFINED. Don't present it
                 * (would trip VUID-VkPresentInfoKHR-pImageIndices-01430). Force a recreate. */
                fprintf(stderr, "[vk] gen-frame submit failed: %s\n", vk_result_str(gen_sr));
                g_vk.swapchain_needs_recreate = 1;
                goto lsfg_present_done;
            }

            VkPresentInfoKHR gen_pi = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &gen_signal,
                .swapchainCount = 1,
                .pSwapchains = &g_vk.swapchain,
                .pImageIndices = &gen_img_idx,
            };
            r = g_vk.pfn_QueuePresent(g_vk.gfx_queue, &gen_pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
                g_vk.swapchain_needs_recreate = 1;
                goto lsfg_present_done;
            }
        }

        lsfg_present_done:
        g_vk.frame_index = (g_vk.frame_index + 1) % FRAMES_IN_FLIGHT;
        g_vk.present_count++;
        goto pace_log;
    }
#endif /* ME_HAVE_LSFG */

    /* --- Normal (non-LSFG) path --- */
    {
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &f->image_available,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &f->cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &signal_sem,
    };
    if (vkQueueSubmit(g_vk.gfx_queue, 1, &si, f->in_flight) != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkQueueSubmit failed\n");
        return -1;
    }
    }


    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &signal_sem,
        .swapchainCount = 1,
        .pSwapchains = &g_vk.swapchain,
        .pImageIndices = &img_idx,
    };
    r = g_vk.pfn_QueuePresent(g_vk.gfx_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        g_vk.swapchain_needs_recreate = 1;
    } else if (r != VK_SUCCESS) {
        fprintf(stderr, "[vk] vkQueuePresentKHR: %s\n", vk_result_str(r));
    }

    pace_log:
    g_vk.frame_index = (g_vk.frame_index + 1) % FRAMES_IN_FLIGHT;
    g_vk.present_count++;

    if (g_vk.pace_log) {
        static LARGE_INTEGER s_qpf = {0}, s_window = {0};
        static int s_window_presents = 0;
        if (!s_qpf.QuadPart) QueryPerformanceFrequency(&s_qpf);
        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (!s_window.QuadPart) s_window = now;
        s_window_presents++;
        double secs = (double)(now.QuadPart - s_window.QuadPart) / (double)s_qpf.QuadPart;
        if (secs >= 1.0) {
            fprintf(stderr, "[vk:pace] presents=%d/s mode=%s ext=%ux%u images=%u\n",
                    s_window_presents, present_mode_str(g_vk.present_mode),
                    g_vk.sc_extent.width, g_vk.sc_extent.height, g_vk.sc_image_count);
            s_window = now;
            s_window_presents = 0;
        }
    }
    return 0;
}

#else  /* !ME_HAVE_VULKAN ---------------------------------------------------- */

int me_vk_init(HWND hwnd, unsigned max_w, unsigned max_h) {
    (void)hwnd; (void)max_w; (void)max_h;
    return -1;
}
void me_vk_shutdown(void) {}
int  me_vk_is_active(void) { return 0; }
int  me_vk_wait_for_present(unsigned timeout_ms) { (void)timeout_ms; return 0; }
int  me_vk_present(const u32 *pixels, unsigned frame_w, unsigned frame_h, unsigned max_w,
                   int cw, int ch, int dx, int dy, int dw, int dh) {
    (void)pixels; (void)frame_w; (void)frame_h; (void)max_w;
    (void)cw; (void)ch; (void)dx; (void)dy; (void)dw; (void)dh;
    return -1;
}
int  me_vk_lsfg_init(const char *dll_path, unsigned width, unsigned height, int multiplier) {
    (void)dll_path; (void)width; (void)height; (void)multiplier; return -1;
}
void me_vk_lsfg_shutdown(void) {}

#endif
