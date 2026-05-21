/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * lsfg_backend_test.cpp — Step B2 test harness
 *
 * Creates an lsfg-vk Instance (which initializes Vulkan on its own device,
 * parses Lossless.dll, and compiles all shaders). We do NOT open a Context
 * (that requires real shared images from the presenter). The goal is simply:
 *
 *   1. Instance() succeeds          → Vulkan init + shader compilation OK
 *   2. Instance destructor succeeds → no leaks / validation errors
 *
 * Run with Lossless.dll in the same directory or pass path as argv[1].
 */

#include "lsfg-vk-backend/lsfgvk.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <windows.h>

// Helper: print to both stdout and stderr, flush both
static void say(const char* msg) {
    fputs(msg, stdout); fflush(stdout);
    fputs(msg, stderr); fflush(stderr);
}

int main(int argc, char **argv) {
    say("[test] Starting lsfg backend test (Step B2)\n");

    std::filesystem::path dll_path;

    if (argc >= 2) {
        dll_path = argv[1];
    } else {
        wchar_t exe_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
        std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();
        dll_path = exe_dir / "Lossless.dll";
        if (!std::filesystem::exists(dll_path))
            dll_path = exe_dir.parent_path().parent_path()
                       / "reference" / "lsfg-vk" / "Lossless.dll";
    }

    if (!std::filesystem::exists(dll_path)) {
        say("[test] ERROR: Lossless.dll not found.\n");
        say("[test]   Pass path as argv[1]\n");
        return 1;
    }

    {
        char msg[512];
        snprintf(msg, sizeof(msg), "[test] Using DLL: %s\n", dll_path.string().c_str());
        say(msg);
    }

    try {
        lsfgvk::backend::Instance instance(
            [](const std::string& name,
               std::pair<const std::string&, const std::string&>,
               const std::optional<std::string>&) -> bool {
                char msg[256];
                snprintf(msg, sizeof(msg), "[test] Considering device: %s\n", name.c_str());
                say(msg);
                return true;
            },
            dll_path,
            true
        );

        say("[test] Instance created successfully!\n");
        say("[test] Shaders compiled. Closing instance...\n");

    } catch (const lsfgvk::backend::error& e) {
        char msg[512];
        snprintf(msg, sizeof(msg), "[test] FAILED (backend::error): %s\n", e.what());
        say(msg);
        return 1;
    } catch (const std::exception& e) {
        char msg[512];
        snprintf(msg, sizeof(msg), "[test] FAILED (std::exception): %s\n", e.what());
        say(msg);
        return 1;
    }

    say("[test] Step B2 OK - backend Instance init + shutdown clean.\n");
    return 0;
}
