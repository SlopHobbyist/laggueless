/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows port: replaced VK_KHR_external_memory_fd with VK_KHR_external_memory_win32.
 * POSIX int fd → Win32 HANDLE throughout. */

#include "lsfg-vk-common/vulkan/image.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <bitset>
#include <optional>
#include <windows.h>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

using namespace vk;

namespace {
    /// create an image
    ls::owned_ptr<VkImage> createImage(const vk::Vulkan& vk,
            VkExtent2D extent, VkFormat format, VkImageUsageFlags usage,
            bool external) {
        VkImage handle{};

        const VkExternalMemoryImageCreateInfo externalInfo{
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
        };
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = external ? &externalInfo : nullptr,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {
                .width = extent.width,
                .height = extent.height,
                .depth = 1
            },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE
        };
        auto res = vk.df().CreateImage(vk.dev(), &imageInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateImage() failed");

        return ls::owned_ptr<VkImage>(
            new VkImage(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyImage](VkImage& image) {
                defunc(dev, image, VK_NULL_HANDLE);
            }
        );
    }
    /// allocate memory for an image (Win32 HANDLE-based external memory)
    ls::owned_ptr<VkDeviceMemory> allocateMemory(const vk::Vulkan& vk, VkImage image,
            std::optional<HANDLE> importHandle, std::optional<HANDLE*> exportHandle) {
        VkDeviceMemory handle{};

        VkMemoryRequirements reqs{};
        vk.df().GetImageMemoryRequirements(vk.dev(), image, &reqs);

        auto mti = vk.findMemoryTypeIndex(
            reqs.memoryTypeBits,
            false
        );
        if (!mti.has_value())
            throw ls::vulkan_error("no suitable memory type found for image");

        const VkMemoryDedicatedAllocateInfoKHR dedicatedInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO_KHR,
            .image = image,
        };
        const VkImportMemoryWin32HandleInfoKHR importInfo{
            .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR,
            .pNext = &dedicatedInfo,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT,
            .handle = importHandle.value_or(nullptr)
        };
        const VkExportMemoryAllocateInfo exportInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = &dedicatedInfo,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
        };
        const void* pNextAlloc{};
        if (importHandle.has_value())
            pNextAlloc = &importInfo;
        else if (exportHandle.has_value())
            pNextAlloc = &exportInfo;
        const VkMemoryAllocateInfo memoryInfo{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .pNext = pNextAlloc,
            .allocationSize = reqs.size,
            .memoryTypeIndex = *mti
        };
        auto res = vk.df().AllocateMemory(vk.dev(), &memoryInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkAllocateMemory() failed");

        res = vk.df().BindImageMemory(vk.dev(), image, handle, 0);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkBindImageMemory() failed");

        if (exportHandle.has_value()) {
            const VkMemoryGetWin32HandleInfoKHR handleInfo{
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
                .memory = handle,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT
            };
            HANDLE h{};
            res = vk.df().GetMemoryWin32HandleKHR(vk.dev(), &handleInfo, &h);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkGetMemoryWin32HandleKHR() failed");
            **exportHandle = h;
        }

        return ls::owned_ptr<VkDeviceMemory>(
            new VkDeviceMemory(handle),
            [dev = vk.dev(), defunc = vk.df().FreeMemory](VkDeviceMemory& memory) {
                defunc(dev, memory, VK_NULL_HANDLE);
            }
        );
    }
    /// create an image view
    ls::owned_ptr<VkImageView> createImageView(const vk::Vulkan& vk,
            VkImage image, VkFormat format) {
        VkImageView handle{};

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1
            }
        };
        auto res = vk.df().CreateImageView(vk.dev(), &viewInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateImageView() failed");

        return ls::owned_ptr<VkImageView>(
            new VkImageView(handle),
            [dev = vk.dev(), defunc = vk.df().DestroyImageView](VkImageView& view) {
                defunc(dev, view, VK_NULL_HANDLE);
            }
        );
    }
}

Image::Image(const vk::Vulkan& vk,
            VkExtent2D extent,
            VkFormat format,
            VkImageUsageFlags usage,
            std::optional<HANDLE> importHandle,
            std::optional<HANDLE*> exportHandle) :
        image(createImage(vk,
            extent, format, usage,
            importHandle.has_value() || exportHandle.has_value()
        )),
        memory(allocateMemory(vk,
            *this->image,
            importHandle, exportHandle
        )),
        view(createImageView(vk,
            *this->image,
            format
        )),
        extent(extent) {
}

Image::Image(const vk::Vulkan& vk,
            VkImage adoptedImage, VkDeviceMemory adoptedMemory,
            VkExtent2D extent,
            VkFormat format) :
        // adopt without ownership: deleter-less owned_ptr<> just deletes the
        // heap allocation, no vk destructor is called.
        image(ls::owned_ptr<VkImage>(new VkImage(adoptedImage))),
        memory(ls::owned_ptr<VkDeviceMemory>(new VkDeviceMemory(adoptedMemory))),
        view(createImageView(vk, adoptedImage, format)),
        extent(extent) {
}
