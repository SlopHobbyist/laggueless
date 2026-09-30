# Free firmware

System files laggueless ships with. build.bat and release.bat copy this
folder into `firmware\` next to the exe.

A file belongs here only if both are true:

1. A core we use can't start games without it.
2. Its license lets us redistribute it (so no BIOS dumps from real hardware).

## What's here

| Folder | For | Source | License |
|---|---|---|---|
| `Machines/MSX - C-BIOS`, `Machines/MSX2 - C-BIOS`, `Machines/MSX2+ - C-BIOS` | blueMSX (MSX) | [C-BIOS](https://cbios.sourceforge.net/) 0.29a, `cbios-0.29a.zip`: ROMs from `roms/`, `config.ini` from `configs/blueMSX/` | 2-clause BSD (`cbios.txt` in each folder) |

C-BIOS is an MSX BIOS written from scratch. It starts cartridge games, but has
no MSX-BASIC and no disk support. blueMSX uses it when the real MSX machines
aren't installed ("'MSX2+' is incomplete, falling back to 'MSX2+ - C-BIOS'").
The C-BIOS machines in libretro's blueMSX system pack are version 0.23, which
hangs on some games (Metal Gear); 0.29a runs them.

## Checked and left out

Tested by starting each console's cores with an empty `firmware\` folder.

Needed, but can't be redistributed (players supply their own):

- PlayStation BIOS (`scph5500/5501/5502.bin`) for Beetle PSX and SwanStation.
  PCSX ReARMed starts games without one.
- ColecoVision OS-7 (`colecovision.rom`) for Gearcoleco. The only open BIOS
  (8bitworkshop's) runs homebrew that doesn't call the BIOS, not commercial games.
- Intellivision `exec.bin` and `grom.bin` for FreeIntv.
- Atari Lynx `lynxboot.img` for Beetle Lynx. Handy starts games without one.

Not needed to start games, so not shipped: Dolphin's `Sys` folder, PPSSPP's
assets, the blueMSX Databases, and the GBA, DS, Dreamcast, 5200, 7800 and
Pokemon Mini BIOSes (those cores have a built-in stand-in or boot without one).
