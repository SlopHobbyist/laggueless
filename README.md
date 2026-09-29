# Laggueless (RetroArch alternative)
#### No confusing menus, just play!

![alt text](https://raw.githubusercontent.com/SlopHobbyist/thumbnails/main/laggueless2.png "Example Screenshot | Playing Mario on the NES")

## Goal
The goal was to create a better loader for emulator cores (libretro). Setting up RetroArch is insanely confusing. Laggueless defaults to reasonable settings, so you can focus on simply playing games. All design decisions aim to reduce lag everywhere possible.

## Features
- Super low latency vulkan rendering. Tested for weeks with [this](https://www.osrtt.com/product-page/osltt-cs) open source hardware.
- Perfect pixel scaling for stutter free scrolling
- Gsync/freesync monitor sync

## Download
You can grab the latest version on the [Releases Page](https://github.com/SlopHobbyist/laggueless/releases)

## How to use
1. [Unzip](https://www.7-zip.org/) the folder wherever you want, it's portable
2. Double click `laggueless.exe` to open
3. To download all cores, go to `Cores > Download Cores...`
4. All done! Drag your favorite game onto the window and start playing!

## Supported Consoles
*If your console is missing, try loading its RetroArch core. It very likely works.*

`NES` `SNES` `Game Boy` `Game Boy Color` `Game Boy Advance` `DS` `3DS` `N64` `GameCube` `Wii` `Virtual Boy` `Pokemon Mini` `Genesis` `32X` `Master System` `Game Gear` `SG-1000` `Dreamcast` `TurboGrafx-16`, `SuperGrafx` `PS1` `PSP` `Atari 2600` `Atari 5200` `Atari 7800` `Atari Lynx` `Atari Jaguar` `Neo Geo Pocket` `Neo Geo Pocket Color` `WonderSwan` `WonderSwan Color` `ColecoVision` `Intellivision` `Vectrex` `MSX`

## Speedrunning
Laggueless deliberately **does not** support the following, so it can be used in many speedrun leaderboards and competitions.
Obviously check with your organizers first before using.
- Save States
- Pausing (must use in-game pause buttons if available)
- Simultanious L+R U+D inputs, sends neutral instead
- Turbo Buttons
- Game Genie / Game Shark / Gecko Codes
- Muting Audio

## Roms
This repo does not enable piracy. Users must provide their own ROM files.

## LSFG Frame Generation

Use *"fake frames"* to achieve higher framerates (300+fps?!). LSFG frame generation is optional and requires [Lossless Scaling](https://store.steampowered.com/app/993090/Lossless_Scaling/) on Steam. This is the only feature in the whole program that *adds lag*.

## Building

For detailed build instructions, see [build.md](build.md) (not written by me).

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See the [LICENSE](LICENSE) file for the full text.

This project links against libretro cores, which are distributed under their own respective licenses. Cores are not included in this repository and remain the property of their respective authors.

## Disclosure

> **Note**: Except for this readme and any image assets, this entire project was written by AI (Claude Code: Sonnet 4.5, Opus 5.5, and Antigravity: Gemini 3 Pro (High)). All code, architecture decisions, and implementation details were generated through AI assistance.
>
> I am a strong advocate for never mixing generated code into real repos.
> Projects like these should clearly disclose as such.
> I even have separate Github accounts for AI projects vs handwritten ones.