# laggueless todo

Findings from the per-console test run (`../big_logs.txt`, 2026-09-29).
Suggested order: 1 and 3 first (they cover five broken games), then 2 with the
pre-load BIOS check (covers the messages for 9-12), then 4 and 6.

## A. Frontend bugs

- [x] **1. Route shared extensions by file header, not extension alone** ([src/rom_cores.c](src/rom_cores.c))
  Each extension belonged to exactly one console, so shared extensions went to the wrong core. Now `exts` is still the one console that owns a type (File Associations are unchanged), and the new `also_exts` lists the other consoles that use it. When a type has several candidates, `header_is` decides from the file header.
  - [x] Mario Kart Wii (`.iso`) now goes to Dolphin (Wii magic at 0x18). Before, it went to PPSSPP, which crashed.
  - [x] Galaga 7800 (`.bin`) has no header (it's a headerless dump), so the Pick Console dialog asks. Picking Atari 7800 loads it in ProSystem.
  - [x] Crazy Taxi 2 (`.cue`) now goes to Flycast (`SEGA SEGAKATANA` in the data track).
  - [x] Metal Gear MSX (`.rom`) now goes to blueMSX, but blueMSX's `retro_load_game` returns false because `firmware/Machines` and `firmware/Databases` are missing (see new item 16).
  - [x] Headers checked: Wii/GC magic, `PSP GAME` / `PLAYSTATION` in the ISO volume descriptor (2048-byte and raw 2352-byte sectors), `SEGA` at 0x100/0x101, `ATARI7800` at 1, `AA55`/`55AA` for ColecoVision, `AB` for MSX, the `A8` `.rom` header for Intellivision, `SEGA SEGAKATANA` in a cue's first data track.
  - [x] Shared types: `.iso` (PSP, GameCube/Wii, PS1), `.cue` (PS1, Dreamcast), `.bin` (Mega Drive, 2600, 5200, 7800, ColecoVision, Intellivision), `.rom` (MSX, ColecoVision, Intellivision).
  - [x] When the header doesn't settle it, the Pick Console dialog opens (on the UI thread; the running game isn't paused). The pick is kept for Hard Reset and Power On. It also works when a ROM is opened from the command line or by double-clicking.
  - [ ] Not done: `.chd` and `.m3u` still go to PS1 only. A Dreamcast `.chd` would need CHD metadata parsing, and an `.m3u` could follow its first entry.
  - [ ] Maybe: remember the pick per file so Open Recent doesn't ask again for a headerless `.bin`.

- [ ] **2. Fix the missing-firmware warning** ([src/main.c](src/main.c#L130-L150))
  - [ ] It matches the word "firmware" anywhere in a log line, and the system folder is `build\firmware`, so it fires on paths (Stella system directory, Genesis Plus GX's optional `ggenie.bin` hint, Dolphin Sys path, PPSSPP lang file).
  - [ ] It misses real errors: FreeIntv prints straight to stdout, not through the log callback.
  - [ ] It only prints to the console, so users who open a ROM by double-clicking never see it.
  - [ ] Replace it with a check of each console's required BIOS files before loading, plus a dialog when one is missing.

- [ ] **3. Input callback ignores device subclasses and analog buttons** ([src/main.c](src/main.c#L1126-L1139))
  - [ ] It computes `base = device & RETRO_DEVICE_MASK` but compares the raw `device`, so cores using a subclassed joypad read 0 for every input. Compare `base`.
  - [ ] It returns 0 for `RETRO_DEVICE_INDEX_ANALOG_BUTTON` (analog triggers).
  - [ ] Main suspect for Melee stuck on the title screen. Re-test after fixing, with the env trace on to see which device the core reads.

- [ ] **4. Frame gen crops the picture on Crash Bandicoot** ([src/main.c](src/main.c#L1680))
  - [ ] The frame-gen context is created at the startup base size and never follows later size changes. PS1 games change resolution at runtime.
  - [ ] Rebuild frame gen at the new size, or create it at the core's max size.
  - [ ] Handle `RETRO_ENVIRONMENT_SET_GEOMETRY` (37) and `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` (32). Neither is handled today.

- [ ] **5. Remove the misleading GL-on-Vulkan warning** ([src/main.c](src/main.c#L1669-L1674))
  It says "GL framebuffer not yet bridged to Vulkan; expect a black screen", but the GL readback path exists and Melee's title screen does show.

- [ ] **6. Tony Hawk's Underground 2 Remix crashes when F1 is pressed rapidly**
  - [ ] Re-run from a console and capture the `[crash]` line. It shows whether the crash is in `ppsspp_libretro.dll` or `laggueless.exe`.
  - [ ] First suspect: a race between the aspect change on the emulation thread and PPSSPP's own GL render thread.

## B. Controls and mappings

- [ ] **7. Devil's Crush: "push run button", but Run can't be found in the mappings**
  The PC Engine layout does list Run on Start ([src/consoles.c](src/consoles.c#L69-L72)). Check:
  - [ ] Does the loaded core's library name match the PC Engine rule list?
  - [ ] Does Start actually reach the core? Could be the same bug as #3.
  - [ ] Is a per-core binding overriding the universal controls?

- [ ] **8. Add rumble to the environment calls** (`GET_RUMBLE_INTERFACE`). Dolphin also asks for sensor and microphone support. Harmless, but GameCube rumble is worth having eventually.

## C. Missing BIOS / system files (setup or packaging, plus better messages)

- [ ] **9. Zaxxon (ColecoVision): OS-7 BIOS missing** (`colecovision.rom`, CRC32 3aa93ef3). The ROM is fine (valid `AA55` header). Gearcoleco then calls it "invalid or corrupted", which is misleading. The pre-load check in #2 should catch this.
- [ ] **10. Defender (Intellivision): `exec.bin` and `grom.bin` missing.** FreeIntv keeps running and spams `HALT!` about 85 times. Refuse to load, or show a warning, when they're missing.
- [ ] **11. Dolphin: `firmware/dolphin-emu/Sys` missing** (no `codehandler.bin`, no post-processing shaders). Ship Sys or have the core downloader fetch it. Do this before judging Melee's input.
- [ ] **12. PPSSPP: assets folder missing** (`firmware/PPSSPP/`). Some games need the fonts and language files.
- [ ] **16. blueMSX: `firmware/Machines` and `firmware/Databases` missing.** Metal Gear now reaches blueMSX (after #1), which then fails `retro_load_game` without them. fMSX needs `MSX.ROM`/`MSX2.ROM` etc. instead.

## D. Needs more investigation

- [ ] **13. Space Invaders (Atari 2600): "Unrecognized ROM file type"**
  The ROM looks valid (4 KB, normal 6502 code) and the frontend passes the right path and data.
  - [ ] Try the `stella2023` core.
  - [ ] Try the same DLL in RetroArch, to see whether this `8.0_pre` build is the problem.
  - [ ] Delete the old `saves/stella/stella.ini` ("event version mismatch") and retry.

- [ ] **14. Dolphin can't create a shared GL context**
  "unable to set shared context", "Failed to initialize shader compiler worker thread". We don't support `SET_HW_SHARED_CONTEXT`, so shaders compile on the main thread and stutter the first time each effect appears. Not game-breaking, but it matters for timing.

- [ ] **15. Defender: 60.00 Hz core on a 239.76 Hz display, "expect minor judder"**
  The legal handling works as designed (native speed is kept). Only a problem if the judder is bothersome. The fix on the user side is a 240.000 Hz custom refresh.
