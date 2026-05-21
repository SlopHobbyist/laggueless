# Plan: Vulkan Low-Latency & Performance Parity with D3D11

This plan details the technical steps to optimize the Vulkan presentation path (`src/render_vulkan.c`) in **Laggueless** to achieve input latency and setup overhead parity with the highly optimized D3D11 waitable swap chain. Completing this plan ensures that when LSFG 3.1 frame generation is enabled, the baseline latency and frame pacing are as clean as possible.

---

## Technical Analysis of the Gap

Currently, the Vulkan path lags behind D3D11 in 2D software emulation (e.g., Mesen, Snes9x) due to three architectural factors:

1. **Lack of True Compositor Pacing:** 
   * **D3D11:** DXGI 1.3 waitable swap chain (`SetMaximumFrameLatency(1)`) blocks the CPU until the Desktop Window Manager (DWM) compositor actually releases the front buffer. This caps backpressure at exactly 1 frame and syncs CPU wakeups directly to vblank.
   * **Vulkan:** The current implementation uses a CPU-side `FRAMES_IN_FLIGHT = 1` gated by a standard fence (`vkWaitForFences`). However, this only waits for the GPU to finish rendering—not for the compositor to display the image. The CPU can easily render ahead and queue extra frames in the driver's presentation queue, adding 1 to 2 frames of hidden input latency.

2. **Per-Frame setup & Graphics Pipeline Overhead:**
   * Vulkan currently routes software-core framebuffers through a full graphics pipeline (render passes, descriptor sets, command buffers, viewport setups, and vertex/fragment shaders) to draw a textured quad. For tiny 2D framebuffers (e.g., 256x240, ~240 KB), the setup and driver CPU overhead dominates the actual copy cost.

3. **Compute Swizzle & Sync Barriers:**
   * For LSFG frame generation, the BGRA upload texture must be swizzled to RGBA to avoid motion-vector color corruption. This currently dispatches an explicit compute shader with heavy layout transitions and CPU timeline semaphore wait barriers.

---

## Proposed Changes

### Step 1: Compositor-Level Pacing via `VK_KHR_present_wait`
To match DXGI's waitable swap chain, we will implement compositor-level pacing in Vulkan using standard Vulkan WSI extensions.

* **Modify Adapter Selection & Device Init:**
  * Query and enable `VK_KHR_present_id` and `VK_KHR_present_wait` device extensions in `me_vk_init` [render_vulkan.c:927](file:///c:/Users/stewie/Downloads/multi-emulator/laggueless/src/render_vulkan.c#L927).
  * Enable the `presentId` and `presentWait` features in the Vulkan physical device feature structure.
* **Implement Present Pacing:**
  * Maintain a monotonic `uint64_t present_id` count on the renderer.
  * In `me_vk_present`, chain a `VkPresentIdKHR` structure to the `VkPresentInfoKHR` structure, passing the current `present_id`.
  * In `me_vk_wait_for_present` [render_vulkan.c:1486](file:///c:/Users/stewie/Downloads/multi-emulator/laggueless/src/render_vulkan.c#L1486), instead of waiting on the in-flight fence, call:
    ```c
    if (present_id > 1) {
        vkWaitForPresentKHR(g_vk.device, g_vk.swapchain, present_id - 1, timeout_ns);
    }
    ```
  * **Result:** This sleeps the CPU thread until DWM has physically scanned out the previous frame, achieving absolute latency parity with D3D11.

---

### Step 2: Skip Graphics Pipeline via Direct `vkCmdBlitImage`
For 2D software cores, the input is already a flat CPU-side BGRX buffer. We do not need a fragment shader, descriptor sets, or graphics render passes. We can perform hardware integer scaling directly using Vulkan's highly optimized hardware blitter.

* **Reconfigure Swapchain Usage:**
  * Confirm that `VK_IMAGE_USAGE_TRANSFER_DST_BIT` is enabled on the swapchain [render_vulkan.c:369](file:///c:/Users/stewie/Downloads/multi-emulator/laggueless/src/render_vulkan.c#L369).
* **Bypass Pipeline for Software Cores:**
  * For standard software-core rendering, skip the `vkCmdBeginRenderPass` and `vkCmdDraw` steps.
  * Instead, transition the `upload_image` to `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL`.
  * Transition the acquired swapchain image to `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL`.
  * Call `vkCmdBlitImage` using `VK_FILTER_NEAREST` with:
    * Source region: `(0, 0, frame_w, frame_h)`
    * Destination region: `(dx, dy, dx+dw, dy+dh)` (using the precomputed integer-scaled offsets).
  * Transition the swapchain image to `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` and present.
  * **Result:** This entirely eliminates graphics setup overhead, rendering passes, descriptor updates, constant buffer mapping, and vertex/fragment shader execution, reducing driver CPU footprint to near-zero.

---

### Step 3: Implement Staging Buffer Ring
Currently, Laggueless uses a single persistently mapped staging buffer. If the CPU maps and writes to this buffer while the GPU is still reading from it for the previous frame's `vkCmdCopyBufferToImage`, the driver must either stall the CPU or allocate silent memory.

* **Create a Ring Buffer:**
  * Increase the staging buffer count to 3, matching the number of potential swapchain images.
  * Rotate the active staging buffer index each frame.
  * **Result:** Removes the possibility of CPU-side mapping stalls during rapid successive emulation ticks.

---

### Step 4: Optimize LSFG Swizzle Paths
For frame generation, the BGRA->RGBA swizzle compute shader can be optimized to run with minimal dispatch overhead, or bypassed entirely.

* **Option A: CPU Vectorized Swizzle (Recommended)**
  * Since software core pixel data is copied row-by-row on the CPU in `main.c` [main.c:382](file:///c:/Users/stewie/Downloads/multi-emulator/laggueless/src/main.c#L382), we can perform the R-B swap during this copy loop using SSE/AVX vector intrinsics.
  * This uploads pre-swizzled RGBA data directly to the staging buffer, completely removing the GPU compute shader dispatch, descriptor updates, storage views, and layout barriers in `render_vulkan.c`.
* **Option B: Compute Shader Cache Locality**
  * If keeping the GPU compute shader swizzle, optimize the local size layout from `8x8` to `16x16` or `32x1` to maximize memory controller cache line coalescing on modern architectures (NVIDIA/AMD).

---

## Verification Plan

### Automated & Diagnostic Verification
1. **Timing Telemetry:** Run the emulator with the `--latency-log` flag. Verify that the Vulkan `present` stage and `backpressure_wait` stages match the D3D11 path to within <0.5ms.
2. **GPU Profiling:** Monitor GPU usage during Vulkan play. The bypass of the graphics pipeline (Step 2) should result in a measurable drop in GPU core clocks and power draw for 2D games.
3. **Vulkan Validation:** Run with `LAGGUELESS_VK_VALIDATE=1` enabled in settings.yaml to guarantee that WSI present wait handles, blit layout transitions, and staging memory barriers are 100% compliant.

### Manual Verification
* Load *Super Mario Bros* on the Mesen core. Use a high-speed camera or input-lag testing device to verify that button-press response times on Vulkan are visually and physically identical to GDI and D3D11.
