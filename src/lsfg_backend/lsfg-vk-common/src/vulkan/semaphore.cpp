/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows port: replaced VK_KHR_external_semaphore_fd with Win32 HANDLE equivalent. */

#include "lsfg-vk-common/vulkan/semaphore.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"
#include "lsfg-vk-common/helpers/pointers.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

#include <optional>
#include <windows.h>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_win32.h>

using namespace vk;

namespace {
    /// create a (binary) semaphore
    ls::owned_ptr<VkSemaphore> createSemaphore(const vk::Vulkan& vk, std::optional<HANDLE> importHandle) {
        VkSemaphore handle{};

        const VkExportSemaphoreWin32HandleInfoKHR exportHandleInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
            .dwAccess = GENERIC_ALL
        };
        const VkExportSemaphoreCreateInfo exportInfo{
            .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .pNext = importHandle.has_value() ? &exportHandleInfo : nullptr,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT
        };
        const VkSemaphoreCreateInfo semaphoreInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = importHandle.has_value() ? &exportInfo : nullptr
        };
        auto res = vk.df().CreateSemaphore(vk.dev(), &semaphoreInfo, VK_NULL_HANDLE, &handle);
        if (res != VK_SUCCESS)
            throw ls::vulkan_error(res, "vkCreateSemaphore() failed");

        if (importHandle.has_value()) {
            const VkImportSemaphoreWin32HandleInfoKHR importInfo{
                .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR,
                .semaphore = handle,
                .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT,
                .handle = *importHandle
            };
            res = vk.df().ImportSemaphoreWin32HandleKHR(vk.dev(), &importInfo);
            if (res != VK_SUCCESS)
                throw ls::vulkan_error(res, "vkImportSemaphoreWin32HandleKHR() failed");
        }

        return ls::owned_ptr<VkSemaphore>(
            new VkSemaphore(handle),
            [dev = vk.dev(), defunc = vk.df().DestroySemaphore](VkSemaphore& semaphore) {
                defunc(dev, semaphore, VK_NULL_HANDLE);
            }
        );
    }
}

Semaphore::Semaphore(const vk::Vulkan& vk, std::optional<HANDLE> importHandle)
    : semaphore(createSemaphore(vk, importHandle)) {}
