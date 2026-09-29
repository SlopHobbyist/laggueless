# Laggueless (RetroArch alternative)

![alt text](https://raw.githubusercontent.com/SlopHobbyist/thumbnails/main/laggueless.png "Example Screenshot | Playing Mario on the NES")

## Goal
The goal was to create a libretro core compatible emulator program. Users find RetroArch confusing, so I will force **my** favorite settings so players can focus on simply playing games. All design decisions aim to reduce visual and input lag/delay.

## Features
- "integer (pixel-perfect) scaling" similar to bsnes-mt but for all cores
- "adaptive sync" for gsync/freesync monitors
- lsfg frame gen for higher framerates (optional)

## Supported Platforms
- ⭐ Windows x86-64

## Using the Release
1. Unzip the file with 7zip/winrar
2. Open the folder
3. Copy your Lossless.dll into the **lsfg** folder (optional)
4. Open Command Prompt
5. CD to the folder
6. Run laggueless.exe --help

## Cores
**Cores > Download Cores** downloads every core from the libretro buildbot ([RetroArch_cores.7z](https://buildbot.libretro.com/nightly/windows/x86_64/RetroArch_cores.7z), a few hundred MB, about 2 GB unpacked) and unpacks it into the `cores` folder. It asks first, and the game keeps running while it works.

You can also download cores yourself from [https://buildbot.libretro.com/nightly/windows/x86_64/](https://buildbot.libretro.com/nightly/windows/x86_64/) and place them in the `cores` folder.

## Usage
`laggueless.exe [options] [[<core.dll>] <rom>]`

- **No arguments** (e.g. double-clicking the exe): opens an empty window. Drag a ROM onto it to play.
- **`<rom>`**: plays the ROM in its console's core (see below).
- **`<core.dll> <rom>`**: plays the ROM in a specific core.

Dragging a ROM onto the window loads it at any time, replacing the current game (its save is written first).

#### Example:
`laggueless.exe ".\roms\Super Mario Bros. (World).nes" --vulkan --lsfg`

#### ROM types
Every file type belongs to one console, and each console's games open in the core we picked for it unless you pick another in **Cores > Set Cores**. The core must be in the `cores` folder.

| Console | Extensions | Our core |
| --- | --- | --- |
| NES / Famicom | `.nes` `.fds` `.unf` `.unif` | Mesen (`mesen_libretro.dll`) |
| SNES / Super Famicom | `.sfc` `.smc` `.swc` `.fig` `.bs` `.st` | bsnes (`bsnes_libretro.dll`) |
| Game Boy | `.gb` `.dmg` `.sgb` | Gambatte (`gambatte_libretro.dll`) |
| Game Boy Color | `.gbc` `.cgb` | Gambatte (`gambatte_libretro.dll`) |
| Game Boy Advance | `.gba` `.agb` | mGBA (`mgba_libretro.dll`) |
| Nintendo DS | `.nds` `.dsi` `.ids` | melonDS DS (`melondsds_libretro.dll`) |
| Nintendo 3DS | `.3ds` `.3dsx` `.cci` `.cxi` | Azahar (`azahar_libretro.dll`) |
| Nintendo 64 | `.n64` `.z64` `.v64` `.ndd` | ParaLLEl N64 (`parallel_n64_libretro.dll`) |
| GameCube / Wii | `.gcm` `.gcz` `.rvz` `.wbfs` `.ciso` `.wia` | Dolphin (`dolphin_libretro.dll`) |
| Virtual Boy | `.vb` `.vboy` | Beetle VB (`mednafen_vb_libretro.dll`) |
| Pokemon Mini | `.min` | PokeMini (`pokemini_libretro.dll`) |
| Mega Drive / Genesis | `.md` `.gen` `.smd` `.mdx` `.bin` | Genesis Plus GX (`genesis_plus_gx_libretro.dll`) |
| Sega 32X | `.32x` | PicoDrive (`picodrive_libretro.dll`) |
| Master System | `.sms` | Genesis Plus GX (`genesis_plus_gx_libretro.dll`) |
| Game Gear | `.gg` | Genesis Plus GX (`genesis_plus_gx_libretro.dll`) |
| SG-1000 | `.sg` `.sc` `.mv` | Genesis Plus GX (`genesis_plus_gx_libretro.dll`) |
| Dreamcast | `.gdi` `.cdi` | Flycast (`flycast_libretro.dll`) |
| PC Engine / TurboGrafx-16 | `.pce` | Beetle PCE (`mednafen_pce_libretro.dll`) |
| SuperGrafx | `.sgx` | Beetle PCE (`mednafen_pce_libretro.dll`) |
| PlayStation | `.cue` `.chd` `.ccd` `.toc` `.m3u` `.pbp` | Beetle PSX (`mednafen_psx_libretro.dll`) |
| PlayStation Portable | `.iso` `.cso` | PPSSPP (`ppsspp_libretro.dll`) |
| Atari 2600 | `.a26` | Stella (`stella_libretro.dll`) |
| Atari 5200 | `.a52` | a5200 (`a5200_libretro.dll`) |
| Atari 7800 | `.a78` | ProSystem (`prosystem_libretro.dll`) |
| Atari Lynx | `.lnx` `.lyx` | Handy (`handy_libretro.dll`) |
| Atari Jaguar | `.j64` `.jag` | Virtual Jaguar (`virtualjaguar_libretro.dll`) |
| Neo Geo Pocket | `.ngp` | Beetle NeoPop (`mednafen_ngp_libretro.dll`) |
| Neo Geo Pocket Color | `.ngc` `.ngpc` `.npc` | Beetle NeoPop (`mednafen_ngp_libretro.dll`) |
| WonderSwan | `.ws` `.pc2` | Beetle WonderSwan (`mednafen_wswan_libretro.dll`) |
| WonderSwan Color | `.wsc` `.pcv2` | Beetle WonderSwan (`mednafen_wswan_libretro.dll`) |
| ColecoVision | `.col` `.cv` | Gearcoleco (`gearcoleco_libretro.dll`) |
| Intellivision | `.int` | FreeIntv (`freeintv_libretro.dll`) |
| Vectrex | `.vec` | vecx (`vecx_libretro.dll`) |
| MSX | `.mx1` `.mx2` | blueMSX (`bluemsx_libretro.dll`) |

#### Menu bar
The game never pauses for the menus or their windows (speedrun rules): it keeps running while menus and dialogs are open and while the window is moved or resized. The menu bar hides in fullscreen.

| Menu | Items |
| --- | --- |
| File | Open ROM, Open Recent (last 20 games played), Exit |
| Console | Hard Reset (reload the game), Soft Reset (the console's reset button), Power On / Off |
| Controls | Player 1, Player 2, Hotkeys. Each opens a window listing every control: click a binding to change it, right-click to clear it. Pick keyboard, controller or both, and which controller slot. Default restores the shipped bindings. Player 1/2 edit the running core's own map if it has one in `settings.yaml`, otherwise the map every core shares. |
| Cores | Download Cores (see [Cores](#cores); a progress window shows the download and can cancel it), Set Cores (every console with the core its games open in: click a console to pick another core from the ones that can run it; cores not in the `cores` folder are marked "not installed"). Default restores our picks. Changes apply the next time a game is opened. |
| View | Toggle Full Screen, Frame Gen (Vulkan only; toggling it briefly stalls while the renderer rebuilds), Rendering Backend (takes effect on restart) |

Changes made from the menus are saved to `settings.yaml`, which laggueless rewrites (without comments) when it saves.

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

## Building

For detailed build instructions, see [build.md](build.md).

## Roms
This repo does not enable piracy. Users must provide their own ROM files. Place them in the roms folder.

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

## Disclosure

> **Note**: This entire project was written by AI (Claude Code: Sonnet 4.5, and Antigravity: Gemini 3 Pro (High)). All code, architecture decisions, and implementation details were generated through AI assistance.
>
> I am a strong advocate for never mixing generated code into real repos.
> Projects like these should clearly disclose as such.