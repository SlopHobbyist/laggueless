# Laggueless (RetroArch alternative)

> **Note**: This entire project was written by AI (Claude Code: Sonnet 4.5, and Antigravity: Gemini 3 Pro (High)). All code, architecture decisions, and implementation details were generated through AI assistance.
>
> I am a strong advocate for never mixing generated code into real repos.
> Projects like these should clearly disclose as such.

## Goal
The goal was to create a libretro core compatible emulator program. Users find RetroArch confusing, so I will force **my** favorite settings so players can focus on simply playing games. All design decisions aim to reduce visual and input lag/delay.

## Features
- "integer (pixel-perfect) scaling" similar to bsnes-mt but for all cores
- 'adaptive sync" for gsync/freesync monitors

## Supported Platforms
- ⭐ Windows x86-64

## Usage
`laggueless.exe [options] <core.dll> <rom>`
#### Optional Arguments:
`--no-audio` `disable audio output`
`--gdi` `force GDI for all cores (overrides --d3d11)`
`--d3d11` `use D3D11 present path for 2D cores too. (enables VRR / lower latency, but may tear on non-GSync/FreeSync displays)`
`--vulkan` `use the Vulkan present path. Required for the planned LSFG frame-generation work. Software cores (mesen, snes9x, etc.) render correctly; GL hardware cores are not bridged to Vulkan yet — use --d3d11 for those.`
`--no-vsync` `(Vulkan only) use IMMEDIATE present mode. VRR-smooth if adaptive sync is enabled in the driver/OS; otherwise tearing for lowest latency on fixed-rate displays.`
`--pace-log` `log audio pacing diagnostics (and Vulkan present pacing when --vulkan is on)`
`--timing-log` `log frame timing diagnostics`
`--env-trace` `log libretro environment calls`

#### Vulkan environment variables:
`LAGGUELESS_VK_VALIDATE=1` `enable VK_LAYER_KHRONOS_validation (Vulkan SDK required)`
`LAGGUELESS_VK_NO_VSYNC=1` `same as --no-vsync`
`LAGGUELESS_VK_MAILBOX=1` `prefer MAILBOX present mode (no tearing, replaces queued frame)`
`LAGGUELESS_VK_PACE_LOG=1` `per-second swapchain/present diagnostics`

#### Example:
`build\laggueless.exe --env-trace example-cores\mesen_libretro.dll "C:\Users\stewie\Downloads\laggueless\example-roms\Super Mario Bros. (World).nes" 2>&1`

## Building

GCC (MSYS2 mingw64) is required. To enable the Vulkan present path, install the [LunarG Vulkan SDK](https://vulkan.lunarg.com/sdk/home) before running `build.bat`. The build script auto-detects `VULKAN_SDK` and links `vulkan-1`; without it, `--vulkan` is unavailable but the build still succeeds.

To regenerate the embedded SPIR-V shaders after editing `src/shaders/quad.vert` or `quad.frag`, run `bash tools_gen_spv_header.sh` (requires `glslc` from the Vulkan SDK on PATH).

## Cores
You can download cores here: [https://buildbot.libretro.com/nightly/windows/x86_64/](https://buildbot.libretro.com/nightly/windows/x86_64/)


## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See the [LICENSE](LICENSE) file for the full text.

This project links against libretro cores, which are distributed under their own respective licenses. Cores are not included in this repository and remain the property of their respective authors. Users are responsible for obtaining cores and ROMs legally.