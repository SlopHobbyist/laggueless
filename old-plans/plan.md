# Laggueless Implementation Plan

## Instructions to the LLM coding agent

**STOP after each step and wait for the user to test before continuing.**
Do not chain steps together. After finishing a step:
1. Confirm `build.bat` compiles cleanly.
2. Tell the user exactly what to run and what they should see / try.
3. Wait for explicit "next" / "step N" / "continue" before starting the next step.

Keep changes scoped to the current step. Don't pre-implement scaffolding for later steps. Don't refactor unrelated code. Don't add features not listed for the step.

If a step's design is ambiguous, ask the user before coding rather than guessing.

---

## Current state (already done)
- Win32 window + message pump ([src/platform_win32.c](src/platform_win32.c))
- Integer scaling math + smoke test ([src/integer_scaling.c](src/integer_scaling.c), [src/main.c](src/main.c))
- Core loader stub ([src/core_loader.c](src/core_loader.c)) — returns NULL, not wired up
- GCC build via [build.bat](build.bat)
- libretro cores sitting in [example-cores/](example-cores/) (bsnes, snes9x, mesen, nestopia, mupen64plus, parallel_n64)

---

## Step 1 — libretro headers + core loader (LoadLibrary + GetProcAddress)
**Goal:** load a libretro core DLL, resolve every `retro_*` symbol, expose them through a struct. No callbacks yet, no running.

- Drop `libretro.h` into [include/](include/) (vendor it; don't fetch at build time).
- Fill in [src/core_loader.c](src/core_loader.c):
  - `LoadLibraryA(dll_path)`
  - `GetProcAddress` for: `retro_api_version`, `retro_get_system_info`, `retro_get_system_av_info`, `retro_set_environment`, `retro_set_video_refresh`, `retro_set_audio_sample`, `retro_set_audio_sample_batch`, `retro_set_input_poll`, `retro_set_input_state`, `retro_init`, `retro_deinit`, `retro_load_game`, `retro_unload_game`, `retro_run`, `retro_serialize_size`, `retro_serialize`, `retro_unserialize`.
  - Store function pointers in `struct me_core`.
- In [src/main.c](src/main.c), accept a DLL path on argv[1], load it, print `retro_api_version()` and `retro_get_system_info()` fields, then unload and exit (no window yet for this step).

**User test:** `laggueless.exe example-cores\snes9x_libretro.dll` prints API version 1 and the core's name/version/extensions.

---

## Step 2 — environment callback + load a ROM
**Goal:** set callbacks well enough to call `retro_load_game` on a real ROM and reach `retro_run` once.

- Implement the 5 libretro callbacks. For step 2 they can be near-stubs:
  - `environment`: handle `RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY`, `_GET_SAVE_DIRECTORY`, `_SET_PIXEL_FORMAT` (accept XRGB8888 and RGB565), `_GET_LOG_INTERFACE`, `_GET_VARIABLE` (return false). Reject everything else by returning false.
  - `video_refresh`, `audio_sample`, `audio_sample_batch`: no-op (just record that they were called + last framebuffer dims).
  - `input_poll`, `input_state`: return 0.
- Accept ROM path on argv[2]. Read into memory, hand to `retro_load_game` via `struct retro_game_info`.
- Call `retro_run` 60 times, then `retro_unload_game` + `retro_deinit`.

**User test:** `laggueless.exe <core.dll> <rom>` runs without crashing and prints reported AV info (geometry, fps, sample rate) and that video_refresh was called N times.

---

## Step 3 — present video to the window (GDI StretchDIBits, no scaling yet)
**Goal:** see the game on screen, even if stretched and ugly.

- Re-enable window creation in [src/main.c](src/main.c).
- In `video_refresh`, copy the framebuffer (handle both XRGB8888 and RGB565 → convert to BGRX for GDI) into a backbuffer owned by main.
- After each `retro_run`, `StretchDIBits` the backbuffer to the client area.
- Run continuously (loop until window closes) instead of 60 frames.
- Timing: just `Sleep` to roughly match `av_info.timing.fps` for now. Real pacing comes in step 6.

**User test:** ROM boots and is visible in the window. Aspect will be wrong/stretched — that's expected.

---

## Step 4 — integer scaling + 4:3 aspect
**Goal:** wire the existing [src/integer_scaling.c](src/integer_scaling.c) math into the present path.

- On each present, call `me_iscale_ratios` with the current client size, core geometry, and a 4:3 target.
- Letterbox/pillarbox with black borders (clear the rest of the client area).
- Handle `WM_SIZE` so resizing the window recomputes scale.
- Add F11 to toggle borderless fullscreen.

**User test:** Image is pixel-perfect integer-scaled with correct 4:3 aspect, centered with black bars. Resizing keeps it sharp; fullscreen works.

---

## Step 5 — keyboard input mapped to RetroPad
**Goal:** playable with the keyboard.

- In `input_poll`, snapshot keyboard state (GetAsyncKeyState or WM_KEYDOWN/UP table).
- In `input_state`, return the RetroPad button for player 1 from a fixed mapping:
  - Arrows → D-pad, Z=B, X=A, A=Y, S=X, Enter=Start, RShift=Select, Q/W=L/R.
- Player 2 only if it's not extra work; otherwise defer.

**User test:** Game responds to keyboard.

---

## Step 6 — audio out (WASAPI shared mode) as the master clock
**Goal:** correct game speed driven by audio, not `Sleep`.

- Open a WASAPI shared-mode render stream at `av_info.timing.sample_rate` (or resample to device rate if mismatched — start with "fail loud if mismatched" and only resample if needed).
- `audio_sample_batch` writes into a ring buffer.
- Main loop: call `retro_run` whenever the audio buffer has room (this naturally paces the emulator). Remove the `Sleep`.

**User test:** Sound plays, no crackle, game runs at normal speed for several minutes without drift.

---

## Step 7 — adaptive sync presentation
**Goal:** low-latency presentation on G-Sync/FreeSync displays.

- Switch present path to DXGI flip-model swapchain (`DXGI_SWAP_EFFECT_FLIP_DISCARD`, `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`).
- Present with `DXGI_PRESENT_ALLOW_TEARING` and `SyncInterval=0` in fullscreen-borderless.
- Keep GDI path as a fallback for windowed mode if DXGI isn't available.
- Upload the core framebuffer as a texture; draw a single quad using the integer-scaled rect from step 4.

**User test:** On a VRR monitor in fullscreen, no tearing and no judder; input feels tighter than step 6.

---

## Step 8 — simple game-picker menu
**Goal:** launch without command-line args.

- On startup with no argv, scan a `games/` folder (next to the exe). Map extensions to cores (`.sfc/.smc → snes9x`, `.nes → nestopia`, `.n64/.z64 → mupen64plus_next`, etc. — table-driven).
- Render a plain list using GDI text (one entry per line, arrow keys + Enter to select, Esc to quit). Do not pull in a UI library.
- Selected entry → load core + ROM and enter the existing run loop.

**User test:** Double-click the exe, see the game list, pick one, play it.

---

## Step 9 — save states + config file
**Goal:** quality-of-life. Keep it small.

- F5 = save state, F8 = load state to `saves/<rom-basename>.state` using `retro_serialize` / `retro_unserialize`.
- Read a tiny `config.ini` next to the exe for: games folder, fullscreen-on-launch, aspect override (4:3 / 8:7 / core).
- No in-game settings menu — config file only, by design ("force my favorite settings").

**User test:** Save/load state mid-game works across launches. Editing config.ini changes behavior on next run.

---

## Step 10 — port shims
**Goal:** the "afterthought" platforms from the README.

- Windows x86 (32-bit): verify build.bat with `-m32`; cores must match bitness.
- Ubuntu x86-64: add `build.sh`, swap Win32 window for X11 or SDL2-only-for-window, swap WASAPI for ALSA/PulseAudio, swap DXGI for OpenGL/Vulkan or just X11 + tearing-allowed.
- This step is large — break into sub-steps with the user before starting.

**User test:** Each platform builds and runs at least one core.

---

## Out of scope (don't build unless asked)
- Netplay, rewind, cheats, shaders beyond integer scaling, achievement integration, in-game overlay menu, automatic core downloads, ROM scraping/metadata, controller remapping UI.
