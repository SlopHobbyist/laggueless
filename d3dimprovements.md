# D3D11 Path — Why It Loses to GDI for 2D Cores, and How to Fix It

Empirical observation: the GDI present path runs noticeably better than the D3D11 path for software (2D) cores. Here's why, and what to change in [src/render_d3d11.c](src/render_d3d11.c) to close the gap.

## Why GDI currently wins for 2D cores

1. **DWM queues one frame; the D3D11 flip chain queues two.** GDI `StretchDIBits` is composited by DWM at the next vblank — at most one frame held. The current swap chain is created with `BufferCount = 2` ([render_d3d11.c:101](src/render_d3d11.c#L101)) and no frame-latency cap, so the OS can queue an extra buffered frame.

2. **`Present(0, ALLOW_TEARING)` doesn't actually skip buffering on non-VRR displays.** Without GSync/FreeSync active, sync interval 0 produces tearing but the OS still buffers. The DXGI flip queue ends up longer than DWM's single-frame queue.

3. **CPU→GPU upload stall every frame.** D3D11 path does `Map`/`memcpy`/`Unmap` on a `D3D11_USAGE_DYNAMIC` texture ([render_d3d11.c:121-124](src/render_d3d11.c#L121-L124)). With `BufferCount = 2` and no fence-based ring, `Map(WRITE_DISCARD)` can still stall when the GPU hasn't released the previous sampler binding. GDI's kernel blit has no equivalent stall point.

4. **Fixed per-frame draw overhead.** Constant-buffer update + shader draw + Present has more fixed overhead than `StretchDIBits` for tiny framebuffers (256×240 ≈ 240 KB). At this resolution the D3D11 setup cost dominates the actual blit cost.

5. **Integer scaling math is identical** in both paths ([main.c:606-614](src/main.c#L606-L614)). So the latency delta is purely the present pipeline — not the scaler.

## Fixes (ranked by impact)

1. **Waitable swap chain + `SetMaximumFrameLatency(1)`.** Add `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` to swap-chain flags, query `IDXGISwapChain2::GetFrameLatencyWaitableObject()`, and `WaitForSingleObject` on that handle just before building the next frame. Caps the OS queue at one frame and gives a precise present-boundary wake — single biggest win.

2. **Staging texture ring with fence/event.** Replace the single dynamic texture with a small ring (2–3 entries). Round-robin writes so `Map(WRITE_DISCARD)` never stalls on in-flight GPU work. For a 240 KB upload the difference is small, but it removes the worst-case stall.

3. **Drop `Present(0)` with no VRR detection.** On non-VRR displays, `Present(1, 0)` with a waitable chain at max latency 1 is lower-latency than `Present(0, ALLOW_TEARING)` because the OS no longer needs to buffer to handle the missed vblank case. Detect VRR-active at runtime and pick: VRR → `Present(0, ALLOW_TEARING)`; non-VRR → `Present(1, 0)` with waitable.

4. **Use `IDXGISwapChain1::Present1` with `DXGI_PRESENT_DO_NOT_WAIT`** for the polling case — lets the engine detect a busy queue and skip rather than block.

5. **Independent flip / direct flip.** Make the window borderless-fullscreen-sized to exactly cover the output and use `DXGI_SCALING_NONE` so DWM hands off to independent flip (driver/hw scanout path), bypassing the desktop compositor entirely. Same latency profile as legacy exclusive fullscreen.

6. **Avoid the shader pass for SW cores.** For SW cores, the source is already in CPU memory in BGRA. A `CopySubresourceRegion` from a staging texture directly into the back buffer (no draw call, no sampler, no constant buffer) skips most of the per-frame setup. Use the shader path only when scaling needs filtering — but you're using `D3D11_FILTER_MIN_MAG_MIP_POINT` ([render_d3d11.c:134](src/render_d3d11.c#L134)) anyway, so copy+stretch is equivalent.

7. **Drop the constant-buffer Map per frame** if rect/uv hasn't changed (typical between resizes). Trivial CPU win.

8. **Set DXGI maximum frame latency on the device** as a belt-and-suspenders: `IDXGIDevice1::SetMaximumFrameLatency(1)`.

9. **Disable DWM transparency blending** in the window class (`WS_EX_NOREDIRECTIONBITMAP`) when using flip-discard — skips DWM's redirection surface and gets closer to direct flip on windowed mode.

10. **Telemetry.** Log `IDXGISwapChain::GetFrameStatistics` (`PresentCount`, `PresentRefreshCount`, `SyncRefreshCount`, `SyncQPCTime`) per frame to see exactly how many vblanks the present is being delayed by. This is the only way to confirm whether the above changes actually shortened the chain.

## Pragmatic answer

If most users will be on non-VRR displays running 2D cores, the current default (GDI for SW cores, D3D11 for HW cores) is correct — it matches the architecture each path is good at. The D3D11 path was built for the GL-interop case where the texture already lives on the GPU. Fixes 1, 2, and 3 above are the minimum to make D3D11 competitive with GDI for SW cores; without those, keeping `--gdi` as the SW default is the right call.
