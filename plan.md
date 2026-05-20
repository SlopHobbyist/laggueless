# Plan: LSFG 3.1 Frame Generation in Laggueless

## Scope and Ground Rules

This work is split into **two independent plans**. Finish and ship Plan A entirely before starting Plan B. They are not interleaved.

- **Plan A — Vulkan present path.** Add a Vulkan renderer alongside the existing GDI and D3D11 paths. No frame gen. Goal: parity with the D3D11 path (integer scaling, low-latency present, VRR).
- **Plan B — LSFG 3.1 frame generation.** Port the `lsfg-vk-backend` to Windows and wire it into the Vulkan present path from Plan A.

**Target platform:** Windows 10/11 x64 only. No Linux, no Steam Deck.

**Lossless.dll handling:** Users must own Lossless Scaling on Steam. We will document a `lsfg/` folder next to `laggueless.exe` where the user drops their own `Lossless.dll`. We never redistribute the DLL. The loader looks there first, then falls back to `--lsfg-dll=<path>`.

**Input latency:** LSFG inherently adds latency (one real-frame delay + FIFO queuing). To compensate, we will add **optional run-ahead** (re-run the core N frames forward each tick, restore state, present the latest). This is its own follow-up — not part of Plan A or Plan B. Tracked in Plan C below.

**Pause between steps.** After each numbered step, stop and let the user manually test the build before moving to the next step. Do not chain steps. Each step should leave the program in a runnable, shippable state.

---

## Plan A — Vulkan Present Path

Goal: a `--vulkan` flag that uses Vulkan to present software-core frames, with integer scaling and VRR support, matching the D3D11 path's latency characteristics.

### Step A1 — Vulkan SDK + build wiring
- Add Vulkan SDK detection to `build.bat` (look for `VULKAN_SDK` env var).
- Link `vulkan-1.lib`. Add include path.
- Add a `--vulkan` CLI flag in [src/main.c](src/main.c) that for now just logs "vulkan path requested" and exits.
- No rendering yet.

**🛑 Stop. User tests:** build succeeds with and without Vulkan SDK present; `--vulkan` flag is recognized.

### Step A2 — Instance, physical device, logical device
- New files: `src/render_vulkan.c` / `src/render_vulkan.h` mirroring the shape of [src/render_d3d11.h](src/render_d3d11.h).
- `me_vk_init(HWND, max_w, max_h)`: create `VkInstance` (with `VK_KHR_surface`, `VK_KHR_win32_surface`), pick a physical device, create `VkDevice` with a graphics+present queue.
- Log adapter name and Vulkan version on init. No swapchain yet.
- `me_vk_shutdown()` cleans up cleanly.

**🛑 Stop. User tests:** `--vulkan` initializes and shuts down without leaks or validation errors (run with `VK_LAYER_KHRONOS_validation` enabled via env var).

### Step A3 — Swapchain + clear-color present
- Create `VkSurfaceKHR` from the HWND.
- Create a swapchain with `VK_PRESENT_MODE_FIFO_KHR` (default) and 2 images.
- Per-frame: acquire → record a command buffer that clears to a recognizable color (cornflower blue) → submit → present.
- Wire into the main loop so `--vulkan` actually paints the window.

**🛑 Stop. User tests:** window is filled with the clear color, no validation errors, clean shutdown, works on resize.

### Step A4 — Upload + textured quad
- Add a staging buffer + sampled image for the framebuffer (sized `max_w × max_h`, `VK_FORMAT_B8G8R8A8_UNORM`).
- Per-frame: copy CPU pixels into staging, `vkCmdCopyBufferToImage`, layout transitions, sample in a fragment shader, draw a fullscreen quad sized to the integer-scaled destination rect.
- Reuse the integer scaling math from [src/integer_scaling.c](src/integer_scaling.c) — pixel arithmetic, no shader filtering.
- SPIR-V shaders live in `src/shaders/` next to the existing HLSL; generate headers via the existing `tools_gen_shader_header.sh` pattern (extend to handle `.spv`).

**🛑 Stop. User tests:** load a NES core + ROM, confirm integer-scaled output looks identical to the D3D11 path; verify pixel-perfect at native, 2x, 3x scale.

### Step A5 — Low-latency present
- Use `VK_PRESENT_MODE_MAILBOX_KHR` when available, fall back to FIFO.
- Cap in-flight frames at 1 (semaphore + fence pacing, no triple buffering).
- Add a "wait for next present" hook analogous to `me_d3d11_wait_for_present` so `main.c`'s pacing loop works the same.
- Add `--vulkan-pace-log` parity with the existing `--pace-log`.

**🛑 Stop. User tests:** measure frame-to-frame latency with `--pace-log` style output and compare to D3D11 path. Should be within one vblank.

### Step A6 — VRR / adaptive sync
- Detect VRR-capable swapchain. On Windows there is no direct Vulkan VRR query; rely on `VK_PRESENT_MODE_IMMEDIATE_KHR` with adaptive-sync-enabled driver, matching how the D3D11 path uses `ALLOW_TEARING`.
- Document the tradeoff (tearing on non-VRR monitors) the same way as the D3D11 path.

**🛑 Stop. User tests:** on a GSync/FreeSync monitor, confirm no tearing and no judder; on a fixed-rate monitor, confirm acceptable behavior or fall back to FIFO.

### Step A7 — Polish + docs
- Update README with `--vulkan` flag.
- Add `--vulkan` to `build_and_run.bat` test matrix.
- Confirm clean validation-layer output on init, run, resize, shutdown.

**🛑 Stop.** Plan A is done. Ship it. Get a week of real use before starting Plan B.

---

## Plan B — LSFG 3.1 Frame Generation

Prerequisite: Plan A complete and stable. Vulkan present path is the default for cores using `--vulkan`.

Goal: optional `--lsfg` flag that loads the user's `Lossless.dll`, runs LSFG 3.1 between real frames, and presents the generated frames through the swapchain.

### Step B1 — DLL loader + folder convention
- Add `lsfg/` folder lookup next to `laggueless.exe`. If `lsfg/Lossless.dll` exists, use it. Otherwise honor `--lsfg-dll=<path>`. Otherwise fail with a clear message pointing the user at Lossless Scaling on Steam.
- Document this in README. We do **not** ship the DLL.
- Port the PE shader extraction logic from [reference/lsfg-vk/lsfg-vk-backend/src/extraction/](reference/lsfg-vk/lsfg-vk-backend/src/extraction/). The extraction code reads sections out of `Lossless.dll`; on Windows this is `LoadLibraryEx(..., LOAD_LIBRARY_AS_DATAFILE)` instead of `mmap`.

**🛑 Stop. User tests:** running with `--lsfg` and no DLL gives a clear error; with the DLL in `lsfg/` it loads and logs the extracted shader count.

### Step B2 — Backend port to Windows (no integration yet)
- The reference `lsfg-vk-backend` is Linux-only. Major port work:
  - Replace POSIX file descriptors with Win32 HANDLEs throughout the public API in [lsfgvk.hpp](reference/lsfg-vk/lsfg-vk-backend/include/lsfg-vk-backend/lsfgvk.hpp).
  - Replace `VK_KHR_external_memory_fd` with `VK_KHR_external_memory_win32`.
  - Replace `VK_KHR_external_semaphore_fd` with `VK_KHR_external_semaphore_win32`.
  - Replace `VK_KHR_external_memory_capabilities` opaque-fd handle type with the Win32 equivalent (`VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`).
  - Replace gcc visibility attributes with `__declspec(dllexport)` or build as a static lib.
  - Decide: C++ backend as a static lib linked into our C exe, or as its own DLL. Recommend **static lib**.
- Build the backend standalone with a tiny test harness that creates an Instance, opens a Context with dummy images, and closes it. No real frames yet.

**🛑 Stop. User tests:** backend test harness runs, creates a context, generates one frame into a known-color image, dumps the result, closes cleanly. This is the riskiest step — do not move on until it works.

### Step B3 — Shared images between present device and frame-gen device
- LSFG runs on its own `VkDevice` (the reference doc explains why: it needs Vulkan 1.3 for the DXVK-translated shaders, and isolating it is cleaner).
- From Plan A's Vulkan path, create the two source images (`curr`, `next`) as `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT`, export Win32 handles, hand them to the backend's `openContext`.
- Same for the N destination images (where LSFG writes generated frames) and the timeline semaphore.
- After Plan A's normal upload, copy the freshly uploaded image into the appropriate source slot (alternating `curr`/`next` per real frame).

**🛑 Stop. User tests:** with frame gen disabled internally, confirm the shared-image plumbing introduces no perf regression on the real-frame path.

### Step B4 — Frame insertion + FIFO pacing
- Switch the swapchain to **FIFO** when `--lsfg` is active (matches reference behavior — required for synchronization without CPU mutexes).
- Increase swapchain image count by `1 + (LSFG multiplier - 1)`.
- Per real frame: signal timeline, schedule N generated frames, then for each generated frame call `vkAcquireNextImageKHR` + copy LSFG output to swapchain image + `vkQueuePresentKHR`.
- Wire `--lsfg-multiplier=2|3|4` (LSFG 3.1 supports 2x/3x/4x).

**🛑 Stop. User tests:** play Super Mario Bros at native 60Hz on a 120Hz/144Hz monitor with `--lsfg-multiplier=2`. Confirm smooth motion. Expect increased input latency — that's fine for now, Plan C handles it.

### Step B5 — Settings + UI
- Persist `lsfg` enable + multiplier + flow scale + performance mode in [settings.yaml](settings.yaml).
- Add CLI flags: `--lsfg`, `--lsfg-multiplier=N`, `--lsfg-flow=F`, `--lsfg-perf`.
- Document each in README. Be explicit that LSFG adds input latency and link forward to Plan C (run-ahead).

**🛑 Stop. User tests:** settings round-trip; flags override settings; defaults are sane.

### Step B6 — Polish + docs
- README section explaining LSFG is third-party, requires Lossless Scaling on Steam, and `Lossless.dll` goes in `lsfg/`.
- Note tradeoffs honestly: smoother motion, higher input latency, GPU cost.

**🛑 Stop.** Plan B is done. Ship it.

---

## Plan C — Run-ahead (latency compensation)

Out of scope for now. Sketched here so Plan B's docs can reference it.

Run-ahead works by re-running the libretro core N frames forward each real frame, saving/restoring state. The user sees frame `current + N` instead of `current`, hiding emulator-internal latency. Combined with LSFG it can offset most of the inserted-frame latency cost.

Requires libretro `RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT` + serialize/unserialize per frame. Heavy CPU cost — not all cores will keep up. Optional feature, off by default.

This is its own plan. Do not start until Plan B has shipped and gotten real-world use.
