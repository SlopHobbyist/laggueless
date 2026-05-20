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
`--pace-log` `log audio pacing diagnostics`
`--timing-log` `log frame timing diagnostics`
`--env-trace` `log libretro environment calls`
#### Example:
`build\laggueless.exe --env-trace example-cores\mesen_libretro.dll "C:\Users\stewie\Downloads\laggueless\example-roms\Super Mario Bros. (World).nes" 2>&1`

## Cores
You can download cores here: [https://buildbot.libretro.com/nightly/windows/x86_64/](https://buildbot.libretro.com/nightly/windows/x86_64/)


## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See the [LICENSE](LICENSE) file for the full text.

This project links against libretro cores, which are distributed under their own respective licenses. Cores are not included in this repository and remain the property of their respective authors. Users are responsible for obtaining cores and ROMs legally.