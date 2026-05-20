# 3D Core Support Plan

Goal: support libretro cores that render via OpenGL (mupen64plus-next, parallel-n64, PPSSPP, Dolphin GL, Citra GL, Beetle PSX HW GL, etc.) while preserving the existing design goals:

- GSync/FreeSync via D3D11 flip-model + ALLOW_TEARING (unchanged)
- Perfect integer scaling, no shimmer (unchanged — done in the D3D11 presenter)
- Lowest possible video/input latency

Strategy: **the core renders into a GL FBO; we hand that image to the existing D3D11 presenter.** Two transport paths:

1. **Fast path — `WGL_NV_DX_interop2`**: the GL FBO color attachment *is* a D3D11 texture. Zero copy, ~0 ms added latency. Works on NVIDIA and modern AMD; sometimes on Intel.
2. **Fallback path — CPU readback**: `glReadPixels` from the FBO, then `UpdateSubresource` into the D3D11 texture. 1–3 ms added latency at typical N64 resolutions. Universal.

Software cores (NES/SNES/etc.) keep their current path unchanged.

---

## Step 1 — Wire up `RETRO_ENVIRONMENT_SET_HW_RENDER`

In `me_environment_cb`, handle env cmd 14 (`SET_HW_RENDER`). The core passes a `struct retro_hw_render_callback*` with:

- `context_type` — `RETRO_HW_CONTEXT_OPENGL`, `OPENGL_CORE`, `OPENGLES2/3`, or `VULKAN`. Accept GL/GL_CORE; reject Vulkan for now (return false → core falls back to its non-Vulkan path if it has one, otherwise refuses to load, which is fine).
- `version_major`, `version_minor` — minimum GL version the core needs.
- `depth`, `stencil` — whether to attach those to the FBO.
- `bottom_left_origin` — GL's native Y orientation; track this for the present-time flip.
- `cache_context`, `debug_context` — flags we mostly ignore.

We must fill in two function pointers on the struct *before returning true*:

- `get_current_framebuffer` → returns the FBO id the core should bind for its render target.
- `get_proc_address` → returns a GL function pointer by name (wraps `wglGetProcAddress` with a fallback to `GetProcAddress` on `opengl32.dll` for GL 1.1 entry points).

Store the user-provided `context_reset` and `context_destroy` callbacks. We call `context_reset` once after our GL context exists; `context_destroy` on shutdown or device loss.

## Step 2 — Create a hidden GL context

New file: `src/gl_context.c` / `.h`.

We need a real HWND with a pixel format to make WGL happy, but we don't want a second visible window. Two reasonable options:

- Use the existing emulator HWND. Set a GL-compatible pixel format on it. Risk: `SetPixelFormat` can only be called once per HWND; if D3D11 already grabbed the DC it might conflict.
- Create a separate 1×1 hidden HWND just for the GL context. Cleaner, no interaction with the D3D11 window. **Pick this.**

Sequence:

1. `RegisterClass` a tiny class, `CreateWindowEx` it hidden (`WS_POPUP`, no `WS_VISIBLE`).
2. `GetDC`, `ChoosePixelFormat`/`SetPixelFormat` with a minimal `PIXELFORMATDESCRIPTOR` (32-bit color, no depth — we'll attach depth to the FBO, not the default framebuffer).
3. `wglCreateContext` → temp context → `wglMakeCurrent`.
4. Load `wglCreateContextAttribsARB` via `wglGetProcAddress`.
5. Create the *real* context at the version the core asked for, with `WGL_CONTEXT_CORE_PROFILE_BIT_ARB` if `context_type == OPENGL_CORE`.
6. Destroy temp context, make the real one current.
7. Load the GL function pointers we need ourselves: `glGenFramebuffers`, `glBindFramebuffer`, `glFramebufferTexture2D`, `glGenTextures`, `glTexImage2D`, `glTexParameteri`, `glGenRenderbuffers`, `glBindRenderbuffer`, `glRenderbufferStorage`, `glFramebufferRenderbuffer`, `glCheckFramebufferStatus`, `glViewport`, `glReadPixels`, `glPixelStorei`, `glFinish`, `glGetError`, plus the WGL_NV_DX_interop2 entry points.

Keep this module self-contained — `main.c` only sees `me_gl_init`, `me_gl_make_current`, `me_gl_shutdown`, plus the proc-address wrapper to pass to the core.

## Step 3 — Allocate the render target (FBO + color texture + optional depth)

After `retro_get_system_av_info` (we now know `max_width × max_height`), call into a new `me_gl_fbo_create(max_w, max_h, depth, stencil)`:

1. `glGenTextures(1, &color)`; `glBindTexture`; `glTexImage2D(GL_RGBA8, max_w, max_h, …, NULL)`; nearest filtering.
2. If `depth`: `glGenRenderbuffers`; `GL_DEPTH_COMPONENT24` (or `GL_DEPTH24_STENCIL8` if stencil too).
3. `glGenFramebuffers`; bind; `glFramebufferTexture2D(GL_COLOR_ATTACHMENT0, …, color)`; attach depth/stencil if present.
4. `glCheckFramebufferStatus` must equal `GL_FRAMEBUFFER_COMPLETE`. If not, bail loudly — the core would render to garbage otherwise.
5. Stash the FBO id so `get_current_framebuffer` can return it.

The texture is allocated at *max* geometry; per-frame the core renders into a sub-rect (`base_width × base_height`, or whatever `geometry_changed` reports). We track the current render extents from `retro_set_video_refresh`'s `width`/`height` args, exactly like the software path already does.

## Step 4 — Per-frame: call the core, transport the texture

The video callback for HW cores is different. Instead of a CPU buffer, the core passes the sentinel `RETRO_HW_FRAME_BUFFER_VALID` as `data` to signal "I rendered into the FBO you gave me — go look at it."

In `me_video_refresh_cb`:

```
if (data == RETRO_HW_FRAME_BUFFER_VALID) {
    g_frame_w = w; g_frame_h = h;
    transport_gl_to_d3d11(w, h);
    return;
}
// existing software path unchanged
```

`transport_gl_to_d3d11` picks the path chosen at init time:

### 4a. Interop path

At init, if `WGL_NV_DX_interop2` is supported:

1. `wglDXOpenDeviceNV(d3d11_device)` → interop device handle.
2. Create a D3D11 texture (`DXGI_FORMAT_R8G8B8A8_UNORM`, `D3D11_USAGE_DEFAULT`, `BIND_SHADER_RESOURCE`, `MISC_SHARED`).
3. `wglDXRegisterObjectNV(interop_dev, d3d_tex, gl_color_tex, GL_TEXTURE_2D, WGL_ACCESS_WRITE_DISCARD_NV)`.

Per frame:

1. `wglDXLockObjectsNV` — GL now owns the texture; core renders.
2. (Core has already rendered by the time `me_video_refresh_cb` fires.) `wglDXUnlockObjectsNV` — D3D11 now owns it.
3. Hand the D3D11 texture's SRV to the presenter.

### 4b. Readback path

Per frame:

1. `glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo)`.
2. `glPixelStorei(GL_PACK_ALIGNMENT, 4)`.
3. `glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, cpu_buf)`.
   - GL on Windows generally accepts `GL_BGRA` for fast-path readback (matches D3D11's `R8G8B8A8` swizzle when interpreted as BGRA — actually use `GL_RGBA` and let the shader handle swizzle, OR use `DXGI_FORMAT_B8G8R8A8_UNORM` for the staging texture. Pick whichever matches the existing presenter's expected format. The current presenter takes BGRX so `GL_BGRA` is correct.)
4. The buffer is in GL's bottom-left orientation. Either:
   - Flip during copy (CPU cost: one memcpy per row, reverse order), or
   - Flag the presenter to flip Y in the vertex shader (free).
   The presenter flip is better — add a `flip_y` field to the constant buffer.
5. `ID3D11DeviceContext_UpdateSubresource` into the existing presenter texture.

## Step 5 — Teach the D3D11 presenter about the HW path

`render_d3d11.c` currently takes a CPU pointer + dims and uploads it. Two small additions:

- **`me_d3d11_present_shared(ID3D11Texture2D *tex, w, h, flip_y)`** — for the interop path. Create an SRV on demand for the shared texture (cache by texture pointer), then run the existing draw with that SRV instead of the internal one. The integer-scaling math is identical.
- **Flip-Y in the vertex shader** — add a `cbuffer` field `flip_y` (0 or 1) that flips the V coordinate. Costs nothing; lets the readback path skip the row-reverse.

The swapchain, ALLOW_TEARING, waitable object, integer scaling — all unchanged. This is the whole point of the design.

## Step 6 — Lifecycle: context_reset, context_destroy, geometry changes

- After GL context + FBO exist and *before* `retro_load_game`: nothing to call yet. The core calls `get_current_framebuffer` lazily.
- Right after `retro_load_game` succeeds: call the core's stored `context_reset`. This is when cores upload their shaders/VBOs.
- If the user later resizes the window: the FBO doesn't care — it's sized to `max_width × max_height`, not the window.
- If `RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO` (cmd 32) or `SET_GEOMETRY` (cmd 37) arrives with larger max dims: destroy the FBO + color tex, recreate at the new max, then call `context_reset` again (cores expect this on a "context lost" event).
- On shutdown: `context_destroy` → `retro_unload_game` → `retro_deinit` → destroy FBO → destroy GL context → destroy hidden HWND.

## Step 7 — Detect and skip Vulkan-only cores cleanly

If the core sets `context_type == RETRO_HW_CONTEXT_VULKAN` and we return false from `SET_HW_RENDER`, mupen64plus-next currently has no fallback and `retro_load_game` returns false (which is what we saw with `Vulkan` in the version string — that build only supports Vulkan). Print a clear message:

```
[hw] core requires Vulkan; this front-end only supports OpenGL.
[hw] try the GL build of this core (look for "GLES3" or no "-Vulkan" suffix).
```

Use a different mupen64plus-next build (the regular non-Vulkan one) to actually test N64.

## Step 8 — Verify

Order matters — get each step working before moving on:

1. `--env-trace` a Vulkan-only core, confirm we see `SET_HW_RENDER` with `context_type=2` and we cleanly reject it.
2. Drop in an OpenGL build of mupen64plus-next, confirm `SET_HW_RENDER` arrives with `context_type=1` and we accept.
3. Confirm `context_reset` fires and `get_current_framebuffer` is called from inside `retro_run`.
4. Readback path first (simpler): see Mario 64's title screen in the window, even if upside-down.
5. Add flip-Y to the shader; orientation correct.
6. Add interop path; A/B with readback by env var (`ME_GL_TRANSPORT=readback|interop`) so we can compare latency and pin regressions.
7. Test with PPSSPP and parallel-n64 to shake out assumptions baked around mupen64plus-next.

## Files touched

- `src/main.c` — `SET_HW_RENDER` handler, HW video-refresh branch, lifecycle calls.
- `src/render_d3d11.c` / `.h` — `present_shared` entry point, flip-Y in the shader/cbuffer.
- `src/gl_context.c` / `.h` — **new.** Hidden HWND, WGL context, function-pointer loading, FBO management, interop registration.
- `src/shaders/` — vertex shader gets a `flip_y` uniform path.
- `Makefile` — link `opengl32.lib`, `gdi32.lib` (already linked).

## What we are explicitly NOT doing in this pass

- Vulkan cores — separate, larger project.
- GL ES emulation via ANGLE — only matters for some mobile cores; defer.
- Shared-fence sync between GL and D3D11 — interop's lock/unlock is good enough at 60 fps; revisit if profiling shows stalls.
- Migrating the software path. It works; leave it.
