/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Windows port: replaced readlink/proc/self with GetModuleFileNameA/GetCommandLineA.
 * Wine detection removed (not relevant on native Windows). */

#include "lsfg-vk-common/configuration/detection.hpp"
#include "lsfg-vk-common/configuration/config.hpp"

#include <array>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

using namespace ls;

namespace {
    // try to match a profile by name
    std::optional<GameConf> matchByName(const std::vector<GameConf>& profiles, const std::string& id) {
        for (const auto& profile : profiles)
            if (profile.name == id)
                return profile;
        return std::nullopt;
    }
    // try to match a profile by id
    std::optional<GameConf> matchById(const std::vector<GameConf>& profiles, const std::string& id) {
        for (const auto& profile : profiles)
            for (const auto& activation : profile.active_in)
                if (id == activation)
                    return profile;
        return std::nullopt;
    }
    // try to match a profile by id
    std::optional<GameConf> matchEndsWithId(const std::vector<GameConf>& profiles, const std::string& id) {
        for (const auto& profile : profiles)
            for (const auto& activation : profile.active_in)
                if (id.ends_with(activation))
                    return profile;
        return std::nullopt;
    }
}

Identification ls::identify() {
    Identification id{};

    // fetch LSFGVK_PROFILE
    const char* override_env = std::getenv("LSFGVK_PROFILE");
    if (override_env && *override_env != '\0')
        id.override = std::string(override_env);

    // fetch process exe path (Windows: GetModuleFileNameA)
    std::array<char, MAX_PATH> buf{};
    const DWORD len = GetModuleFileNameA(nullptr, buf.data(), static_cast<DWORD>(buf.size() - 1));
    if (len > 0) {
        buf.at(static_cast<size_t>(len)) = '\0';
        id.executable = std::string(buf.data());
    }

    // fetch process name (basename of executable)
    if (!id.executable.empty()) {
        const size_t last_sep = id.executable.find_last_of("\\/");
        id.process_name = (last_sep != std::string::npos)
            ? id.executable.substr(last_sep + 1)
            : id.executable;
    }

    return id;
}

std::optional<std::pair<IdentType, GameConf>> ls::findProfile(
        const ConfigFile& config, const Identification& id) {
    const auto& profiles = config.profiles();

    // check for the environment option first
    if (std::getenv("LSFGVK_ENV") != nullptr)
        return std::make_pair(IdentType::OVERRIDE, profiles.front());

    // then override first
    if (id.override.has_value()) {
        const auto profile = matchByName(profiles, id.override.value());
        if (profile.has_value())
            return std::make_pair(IdentType::OVERRIDE, profile.value());
    }

    // then check executable
    const auto exe_profile = matchEndsWithId(profiles, id.executable);
    if (exe_profile.has_value())
        return std::make_pair(IdentType::EXECUTABLE, exe_profile.value());

    // finally, fallback to process name
    if (!id.process_name.empty()) {
        const auto proc_profile = matchById(profiles, id.process_name);
        if (proc_profile.has_value())
            return std::make_pair(IdentType::PROCESS_NAME, proc_profile.value());
    }

    return std::nullopt;
}
