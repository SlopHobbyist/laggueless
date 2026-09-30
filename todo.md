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

- [x] **2. Fix the missing-firmware warning** ([src/main.c](src/main.c), [src/rom_cores.c](src/rom_cores.c))
  - [x] Removed the log-keyword heuristic. On the last pass it falsely warned on GB, GBA, MD, SMS, DS, PS1, Lynx, 2600, 5200 and ColecoVision.
  - [x] Replaced it with `k_core_files`: for each core, the system files it can't do without (names checked against the core DLLs). `open_game` checks them before loading. Covered: Gearcoleco, FreeIntv, Beetle PSX (and HW), Beetle Lynx, blueMSX, Dolphin (`dolphin-emu\Sys`) and PPSSPP. Left out on purpose: cores with a built-in stand-in (Flycast, PCSX ReARMed, PokeMini) or unclear needs (Handy, a5200, fMSX, SwanStation).
  - [x] A dialog names the missing files and the folder. If the game still loads (FreeIntv), it's a warning over the running game. If loading fails (Gearcoleco), it replaces "See the console window". Tested with an empty firmware folder, and no false warnings on the 19-console pass with the real folder.
  - [x] Double-click users now see errors: before the window exists, `me_ui_notify_error` shows the message box directly. A startup load that fails after the window is up leaves the idle window showing why, instead of exiting.
  - [ ] Maybe: check sizes/CRCs (Gearcoleco knows 3aa93ef3) to catch bad dumps, and add `syscard3.pce` for PC Engine CD games only.

- [x] **3. Input callback ignores device subclasses and analog buttons** ([src/main.c](src/main.c))
  - [x] It compared the raw `device` instead of `base`, so subclassed devices (a multitap, for example) read 0. It now compares `base`, and it also answers `RETRO_DEVICE_ID_JOYPAD_MASK`.
  - [x] `RETRO_DEVICE_INDEX_ANALOG_BUTTON` now returns 0x7fff while the bound input is held.
  - [x] Melee: the real cause was different. Dolphin declares its ports as a plain joypad (0x1), but it never read input at all, because laggueless never plugged a controller into any port (`retro_set_controller_port_device` was only called for adapters). `plug_gamepads` now plugs `RETRO_DEVICE_JOYPAD` into each port the core describes after loading, as RetroArch does. Melee now goes from the memory-card prompt to the title screen and into the menus.
  - [x] Regression pass (Start pressed once each): NES, SNES, GB, GBA, MD, SMS, PCE, N64, PS1, 5200, Lynx, ColecoVision, Intellivision, MSX, DS, WonderSwan, NGP and Vectrex all load and read the joypad. The 2600 still fails to load (#13, not caused by this).
  - [x] Env and diagnostic logs now flush each completed line. The Windows CRT treats `_IOLBF` as full buffering, so the last 4 KB was lost on a hard exit.

- [x] **4. Frame gen crops the picture on Crash Bandicoot** ([src/render_vulkan.c](src/render_vulkan.c), [src/main.c](src/main.c))
  - [x] Confirmed: frame gen opened at the 320x240 base size, and Crash then draws 640x236, 640x472 and 640x478 frames, so generated frames showed only the top-left corner.
  - [x] `lsfg_fit` reopens frame gen when a frame is bigger than it (grow only: smaller frames fill the top-left, as a single DS screen already did). Crash reopens 3 times at startup, then stays stable. Screenshots with frame gen on and off have the same framing.
  - [x] `SET_GEOMETRY` (37) is accepted: display and frame gen already follow the drawn size.
  - [x] `SET_SYSTEM_AV_INFO` (32) is accepted while loading, and while running unless the frame would outgrow the backbuffer (that's refused, logged once). **Speed bug found:** Beetle PSX loads at 59.94 fps, then asks for the real 59.826. It was refused, so the core retried every frame (522 times in 30 s) and games ran 0.19% fast. Now the game is re-paced from the next frame (`paced_fps`, which is shared with load, plus a pace base so deadlines don't jump). The timing log shows zero drift across the switch.
  - [ ] Not done: a `SET_SYSTEM_AV_INFO` whose max size outgrows the backbuffer would need the renderers rebuilt. Not seen yet.

- [x] **5. Remove the misleading GL-on-Vulkan warning**
  Removed. Confirmed first: Melee on the Vulkan swapchain printed the warning and displayed fine (GL frames reach Vulkan through `me_gl_fbo_readback_bgra`).

- [ ] **6. Tony Hawk's Underground 2 Remix crashes when F1 is pressed rapidly** (couldn't reproduce yet)
  - [x] Tried: 80 taps at 40 ms, 400 taps at 10 ms and a 3 s hold, on the intro/title (Vulkan). There were 401 aspect changes and no crash.
  - [x] F1 only changes `g_aspect_mode`, which `present()` reads on the emulation thread to size the destination rect (the ratios are always ≥ 1 and clamped to the window). It never touches PPSSPP or GL, so the race theory doesn't fit.
  - [x] Crashes are now also written to `logs\crash.log` (plain Win32 writes, timestamped), with module+offset and whether it was "the emulation thread" or "another thread" (a core's own thread). Tested with a fake core that faults both ways.
  - [ ] Next time it happens, send the `logs\crash.log` line and say whether it was in-game, fullscreen, or with frame gen on.

## B. Controls and mappings

- [x] **7. Devil's Crush: "push run button", but Run can't be found in the mappings** (can't reproduce)
  - [x] Beetle PCE matches the PC Engine rule: the log shows "PC Engine controller (8 inputs)".
  - [x] Start reaches the core: Return goes from "PUSH RUN BUTTON" to Play Mode Select.
  - [x] Controls > Player 1 during the game says "Showing the PC Engine controller" and lists Run on Return / Start. There's no per-core override for Beetle PCE.
  - Likely cause: the dialog was opened with no game running, when it shows the generic names ("Start" rather than "Run").

- [x] **8. Add rumble to the environment calls** (`GET_RUMBLE_INTERFACE`)
  - [x] `me_set_rumble_state` sends each player's strong and weak motors to the XInput slot that player uses (not for keyboard-only players). `me_xinput_rumble` only calls `XInputSetState` when the speeds change, and it's safe from a core's own thread. Motors stop when a game closes.
  - [x] Dolphin, N64 and PS1 get the interface and still run. Not felt yet: no controller was connected here, so try it with a pad (Dolphin's "Controller Rumble" core option has to be on).
  - [ ] Sensors and microphone (Dolphin asks) are still unsupported. They're harmless.

## C. Missing BIOS / system files (setup or packaging, plus better messages)

Update: the files for 9, 10, 11, 12 and 16 are now in `build/firmware`. Zaxxon, Defender and Metal Gear now load, and Melee runs with Dolphin's Sys folder. The missing-file message is done too (#2), so 9, 10, 11, 12 and 16 only need the files shipped or downloaded for other users.

- [ ] **9. Zaxxon (ColecoVision): OS-7 BIOS missing** (`colecovision.rom`, CRC32 3aa93ef3). The ROM is fine (valid `AA55` header). Gearcoleco then calls it "invalid or corrupted", which is misleading. The pre-load check in #2 should catch this.
- [ ] **10. Defender (Intellivision): `exec.bin` and `grom.bin` missing.** FreeIntv keeps running and spams `HALT!` about 85 times. Refuse to load, or show a warning, when they're missing.
- [ ] **11. Dolphin: `firmware/dolphin-emu/Sys` missing** (no `codehandler.bin`, no post-processing shaders). Ship Sys or have the core downloader fetch it. Do this before judging Melee's input.
- [ ] **12. PPSSPP: assets folder missing** (`firmware/PPSSPP/`). Some games need the fonts and language files.
- [ ] **16. blueMSX: `firmware/Machines` and `firmware/Databases` missing.** Metal Gear now reaches blueMSX (after #1), which then fails `retro_load_game` without them. fMSX needs `MSX.ROM`/`MSX2.ROM` etc. instead.

## D. Needs more investigation

- [x] **13. Space Invaders (Atari 2600): "Unrecognized ROM file type"**
  - [x] The ROM is a known-good dump (MD5 72ffbef6..., Atari CX2632). Stella 2023 and Stella 2014 both load it; only the buildbot's `stella` (8.0_pre) fails. It also fails with a plain path (`si.a26`) and as `.bin`, and its env trace shows nothing refused beforehand. The error sits beside 8.0's new file-type checks (`.mp3`/`.wav`), so it's the pre-release core. There was no stale `stella.ini`.
  - [x] Made Stella 2023 the Atari 2600 default (Stella 8.0 is still pickable in Set Cores). Space Invaders now opens and plays.
  - [ ] Maybe: try 8.0 in RetroArch, and report it upstream if it fails there too.

- [ ] **14. Dolphin can't create a shared GL context**
  "unable to set shared context", "Failed to initialize shader compiler worker thread". We don't support `SET_HW_SHARED_CONTEXT`, so shaders compile on the main thread and stutter the first time each effect appears. Not game-breaking, but it matters for timing.
  - [x] Tried accepting `SET_HW_SHARED_CONTEXT` (44 | experimental). "unable to set shared context" goes away, but "Failed to create shared context for shader compiling" stays, so Dolphin still can't make its worker contexts from ours. Reverted (no gain, and RetroArch gives such a core a context separate from its own, which we don't).
  - [ ] Real fix: when a core asks, give it a GL context of its own (shared with ours) instead of ours, as RetroArch does. Then check whether Dolphin's workers start. This is a bigger change in `gl_context.c`.

- [ ] **15. Defender: 60.00 Hz core on a 239.76 Hz display, "expect minor judder"**
  The legal handling works as designed (native speed is kept). Only a problem if the judder is bothersome. The fix on the user side is a 240.000 Hz custom refresh.
