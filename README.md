# Laggueless (RetroArch alternative)

> **Note**: This entire project was written by AI (Claude Code: Sonnet 4.5, and Antigravity: Gemini 3 Pro (High)). All code, architecture decisions, and implementation details were generated through AI assistance.
>
> I am a strong advocate for never mixing generated code into real repos.
> Projects like these should clearly disclose as such.

## Goal
The goal was to create a libretro core compatible emulator program. Users find RetroArch confusing, so I will force **my** favorite settings so players can focus on simply playing games. All design decisions aim to reduce visual and input lag/delay.

## Features
- "integer (pixel-perfect) scaling" similar to bsnes-mt but for all cores
- "adaptive sync" for gsync/freesync monitors
- lsfg frame gen for higher framerates (optional)

## Supported Platforms
- ⭐ Windows x86-64

## Usage
`laggueless.exe [options] <core.dll> <rom>`
#### Optional Arguments:

| Flag | Description |
| --- | --- |
| `--no-audio` | disable audio output |
| `--gdi` | force GDI for all cores (overrides `--d3d11`) |
| `--d3d11` | use D3D11 present path for 2D cores too. (enables VRR / lower latency, but may tear on non-GSync/FreeSync displays) |
| `--vulkan` | use the Vulkan present path. Required for the planned LSFG frame-generation work. Software cores (mesen, snes9x, etc.) render correctly; GL hardware cores are not bridged to Vulkan yet — use `--d3d11` for those. |
| `--no-vsync` | (Vulkan only) use IMMEDIATE present mode. VRR-smooth if adaptive sync is enabled in the driver/OS; otherwise tearing for lowest latency on fixed-rate displays. |

#### Logging:
| Flag | Description |
| --- | --- |
| `--lsfg` | enable LSFG 3.1 frame generation (requires `--vulkan`; see LSFG section below). |
| `--lsfg-dll=<path>` | explicit path to Lossless.dll (overrides the `lsfg/` folder search). |
| `--pace-log` | log audio pacing diagnostics (and Vulkan present pacing when `--vulkan` is on) |
| `--timing-log` | log frame timing diagnostics |
| `--env-trace` | log libretro environment calls |

#### Vulkan environment variables:

| Variable | Description |
| --- | --- |
| `LAGGUELESS_VK_VALIDATE=1` | enable `VK_LAYER_KHRONOS_validation` (Vulkan SDK required) |
| `LAGGUELESS_VK_NO_VSYNC=1` | same as `--no-vsync` |
| `LAGGUELESS_VK_MAILBOX=1` | prefer MAILBOX present mode (no tearing, replaces queued frame) |
| `LAGGUELESS_VK_PACE_LOG=1` | per-second swapchain/present diagnostics |

#### Example:
`release\laggueless.exe release\cores\mesen_libretro.dll "release\roms\Super Mario Bros. (World).nes" --vulkan --lsfg`

## Using the Release
1. Unzip the file with 7zip/winrar
2. Open the folder
3. Copy your Lossless.dll into the **lsfg** folder (optional)
4. Open Command Prompt
5. CD to the folder
6. Run laggueless.exe --help

## Building

For detailed build instructions, see [build.md](build.md).

## Cores
You can download cores here: [https://buildbot.libretro.com/nightly/windows/x86_64/](https://buildbot.libretro.com/nightly/windows/x86_64/)
Place them in .\cores\

## Roms
This repo does not enable piracy. Users must provide their own ROM files. Place them in .\roms\

## LSFG 3.1 Frame Generation

LSFG frame generation is optional and requires [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) on Steam. **We do not distribute Lossless.dll.**

To enable LSFG:

1. Find `Lossless.dll` in your Lossless Scaling Steam installation folder.
2. Create a `lsfg/` folder next to `laggueless.exe` (i.e. `build\lsfg\`).
3. Copy `Lossless.dll` into `build\lsfg\Lossless.dll`.
4. Run with `--lsfg --vulkan` (Vulkan is required for frame gen).

Alternatively, point directly at the DLL: `--lsfg-dll="C:\path\to\Lossless.dll"`.

> **Note:** LSFG adds input latency (one real-frame delay + FIFO queuing). This is inherent to the frame generation technique. Try using run-ahead latency to compensate.

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See the [LICENSE](LICENSE) file for the full text.

This project links against libretro cores, which are distributed under their own respective licenses. Cores are not included in this repository and remain the property of their respective authors. Users are responsible for obtaining cores and ROMs legally.