/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows port: uses %ProgramFiles(x86)%\Steam and %LOCALAPPDATA%\Steam
 * instead of Linux XDG / home paths. */

#include "lsfg-vk-common/helpers/paths.hpp"
#include "lsfg-vk-common/helpers/errors.hpp"

#include <cstdlib>
#include <filesystem>
#include <vector>
#include <windows.h>

std::filesystem::path ls::findShaderDll() {
    /* Typical Steam paths on Windows. Try common install roots. */
    const std::vector<std::filesystem::path> ROOTS = [] {
        std::vector<std::filesystem::path> roots;
        const char* pf86 = std::getenv("ProgramFiles(x86)");
        if (pf86 && *pf86) roots.emplace_back(std::filesystem::path(pf86) / "Steam");
        const char* pf   = std::getenv("ProgramFiles");
        if (pf   && *pf)   roots.emplace_back(std::filesystem::path(pf) / "Steam");
        const char* local = std::getenv("LOCALAPPDATA");
        if (local && *local) roots.emplace_back(std::filesystem::path(local) / "Programs" / "Steam");
        return roots;
    }();

    for (const auto& root : ROOTS) {
        auto full = root / "steamapps" / "common" / "Lossless Scaling" / "Lossless.dll";
        if (std::filesystem::exists(full))
            return full;
    }

    // fallback to same directory
    auto local = std::filesystem::current_path() / "Lossless.dll";
    if (std::filesystem::exists(local))
        return local;

    throw ls::error("unable to locate Lossless.dll; place it in lsfg\\Lossless.dll next to the exe, or use --lsfg-dll=");
}
