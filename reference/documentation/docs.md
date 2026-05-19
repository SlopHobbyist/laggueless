# bsnes-mt: Integer Scaling & Adaptive Sync

Notes for porting these two features to other emulator / game projects. Source is `bsnes-mt` (a fork of bsnes by Marat Tanalin).

---

## 1. Integer (Pixel-Perfect) Scaling

### What it is
Given a render area (window/screen) of `areaWidth × areaHeight` and a source image of `imageWidth × imageHeight`, produce an output rectangle whose dimensions are the source dimensions multiplied by an **integer** ratio — so every source pixel maps to an exact NxN block of output pixels. No bilinear smear, no uneven pixel sizes ("shimmer"). Supports optional aspect-ratio correction where the X and Y ratios can differ (rectangular pixels) so e.g. a 256×224 SNES frame can be displayed at a true 4:3 aspect on a 16:9 screen without losing pixel-grid uniformity vertically.

### Where it lives
- Self-contained library: [bsnes-mt/integer-scaling/IntegerScaling.h](bsnes-mt/bsnes-mt/integer-scaling/IntegerScaling.h), [IntegerScaling.cpp](bsnes-mt/bsnes-mt/integer-scaling/IntegerScaling.cpp) — namespace `MaratTanalin`. Pure math, zero deps beyond `<cmath>` / `<cstdint>`. Drop-in portable.
- bsnes-specific wrapper: [bsnes-mt/scaling.cpp](bsnes-mt/bsnes-mt/scaling.cpp), [scaling.h](bsnes-mt/bsnes-mt/scaling.h) — namespace `bsnesMt::scaling`. Knows SNES constants (256×224, 256×240 overscan, 8:7 PAR, 4:3 display AR).
- Call site that wires it into the renderer: [bsnes/target-bsnes/program/_viewport.cpp](bsnes-mt/bsnes/target-bsnes/program/_viewport.cpp).

### The core algorithm

`IntegerScaling::calculateRatio(areaW, areaH, imgW, imgH)` — square-pixel case:

```
if (areaH * imgW < areaW * imgH)   // image is "taller" relative to area → bound by height
    ratio = areaH / imgH
else                                // bound by width
    ratio = areaW / imgW
clamp ratio >= 1
```
Integer division is the entire trick. `calculateSize` just multiplies the source dims by that ratio.

`IntegerScaling::calculateRatios(...)` — aspect-corrected case (different X/Y ratios):

1. Compute the maxima independently: `maxRatioX = areaW / imgW`, `maxRatioY = areaH / imgH`.
2. Decide which axis to pin to its max (axis A) and which to search (axis B), based on whether the resulting `maxWidth*aspectY` is less than or greater than `maxHeight*aspectX` (i.e. which axis would overshoot the target aspect first).
3. For axis B, compute the fractional ratio that would give the *exact* desired aspect: `ratioBFract = maxSizeA * aspectB / aspectA / imageSizeB`. The true answer is non-integer.
4. Test both `floor(ratioBFract)` and `ceil(ratioBFract)`. For each, compute the resulting pixel aspect ratio error vs. the target (`errorFloor`, `errorCeil`). Pick the one with smaller aspect error. If they're within 0.001 of each other, fall back to picking whichever ratio is numerically closer to `ratioA` (favors square-ish pixels as a tiebreaker).
5. Clamp both ratios to >= 1 and return.

This yields integer scaling on **both** axes while honoring aspect correction as closely as one can without dropping integer-ness.

### "Perfect-Y" variant (the one bsnes actually uses)

`calculateSizeCorrectedPerfectY(areaW, areaH, imgH, aspectX, aspectY)` — integer ratio on Y only; X is a fractional `round(imageWidth * ratio)`. Use case: scanline filters need a uniform vertical pixel grid (otherwise scanlines break), but horizontal stretching by a fraction is visually fine on a modern LCD. This is what `outputSetting == "Pixel-Perfect"` selects in [_viewport.cpp:69](bsnes-mt/bsnes/target-bsnes/program/_viewport.cpp#L69).

### How it's wired into rendering

[`Program::viewportSize`](bsnes-mt/bsnes/target-bsnes/program/_viewport.cpp#L48) is called every frame. It reads the current video output area via `video.size()`, then dispatches on the user setting:

| Setting          | Function                          | Behavior                                       |
| ---------------- | --------------------------------- | ---------------------------------------------- |
| `Stretch`        | (none)                            | Fill the whole area; no aspect preservation.   |
| `Center`         | `calculateScaledSizeCenter`       | Calls `calculateSizeCorrectedPerfectY`.        |
| `Pixel-Perfect`  | `calculateScaledSizePerfect`      | Calls `calculateSizeCorrected` (integer X & Y).|
| `Scale`          | `calculateScaledSizeScale`        | Fractional scale, preserves AR (no integer).   |

The computed `scaledWidth/scaledHeight` is then passed to `video.output(outputWidth, outputHeight)` after `video.acquire/release`. The rest of the area around the rect is letterboxed/pillarboxed by the video driver.

### Porting checklist
1. Copy `IntegerScaling.{h,cpp}` verbatim — no changes needed.
2. Every frame, query the current drawable size.
3. Call `calculateSizeCorrected` (or `…PerfectY` if you have scanlines or want a fractional X for true aspect) with your source image dims and the target pixel aspect (use 1:1 for square pixels, or your platform's PAR).
4. Position the returned rect centered in the drawable, draw your texture into it with **nearest-neighbor** sampling. The whole point is wasted if you use linear filtering.
5. Recompute every frame — it's a few divides, and it handles window resize automatically.

---

## 2. Adaptive Sync (the "smooth on G-Sync / FreeSync" mode)

### What it is
A **preset** — not a new sync primitive — that configures bsnes's existing video/audio sync toggles into the one combination that lets a variable-refresh-rate (VRR) monitor drive the pacing. The emulator emits frames at the SNES's native rate (~60.0988 Hz NTSC / 50.007 Hz PAL), and the monitor refreshes exactly when each frame arrives. No tearing, no judder from 60.0988→60.000 Hz mismatch, no audio resampling artifacts.

### Where it lives
Pure UI / settings glue. There is **no** dedicated "adaptive sync" code path in the rendering pipeline — the magic is entirely in which of the four sync flags are on/off.

- Preset button handler: [bsnes/target-bsnes/settings/_drivers.cpp:163](bsnes-mt/bsnes/target-bsnes/settings/_drivers.cpp#L163) (`adaptiveSyncMode.onActivate`).
- The four flags it toggles live in `settings.video.exclusive`, `settings.video.blocking`, `settings.audio.blocking`, `settings.audio.dynamic` — see [settings.cpp:67-93](bsnes-mt/bsnes/target-bsnes/settings/settings.cpp#L67-L93).
- Applied to the actual drivers in [program/_video.cpp:43-47](bsnes-mt/bsnes/target-bsnes/program/_video.cpp#L43-L47) and [program/_audio.cpp:48-54](bsnes-mt/bsnes/target-bsnes/program/_audio.cpp#L48-L54).
- User-facing copy explaining the rules: [translations/en.txt:369-388](bsnes-mt/bsnes-mt/translations/en.txt#L369-L388).

### What the preset actually does
When the user clicks "Adaptive Sync":

```
videoExclusiveToggle  → ON     (exclusive full-screen if driver supports it)
videoBlockingToggle   → OFF    (video.setBlocking(false): do NOT vsync-wait in the emulator)
audioBlockingToggle   → ON     (audio.setBlocking(true):  emulator blocks on the audio queue)
audioDynamicToggle    → OFF    (no dynamic rate-control resampling)
```

Precondition (validated before applying): the current audio driver must support blocking — otherwise it shows the `AdaptiveSync.failure` error and bails. Audio-blocking is the linchpin; without it, nothing throttles the emulator.

### Why those four settings produce smooth VRR

1. **Audio is the clock.** With `audio.blocking = true` and `audio.dynamic = false`, the emulator runs as fast as it can until the audio output queue is full, then blocks on the audio API. Audio plays at its true rate, so the emulator is paced to exactly the SNES's native frame rate (because audio samples per frame is fixed). No drift, no resampling.
2. **Video does not wait.** With `video.blocking = false`, the renderer never vsync-blocks. Each frame is presented to the GPU the instant it's ready.
3. **VRR monitor does the sync.** A G-Sync / FreeSync display will refresh **when the frame arrives** rather than at a fixed 60 Hz cadence. So the presentation cadence becomes: "audio queue allows next frame → emulator produces frame at ~60.0988 Hz → GPU presents immediately → monitor refreshes immediately." No tearing (because the monitor waits for the buffer flip rather than the buffer flip waiting for the monitor), and no 60→60.0988 beat frequency.
4. **Exclusive full-screen** is recommended because on Windows the desktop compositor (DWM) will otherwise force its own vsync-like cadence, defeating VRR. The success dialog ([en.txt:383](bsnes-mt/bsnes-mt/translations/en.txt#L383)) literally says: *"Adaptive sync works best in exclusive full-screen mode. Use the lowest audio latency setting your system can manage. A G-Sync or FreeSync monitor is required. Adaptive sync must be enabled in your driver settings panel."*

### Contrast with the other preset
"Dynamic Rate Control" is the alternative for fixed-60Hz monitors: `video.blocking = true`, `audio.blocking = false`, `audio.dynamic = true`. There, vsync drives pacing and audio is resampled by tiny amounts to absorb the 60 ↔ 60.0988 mismatch. Adaptive Sync is strictly better when the hardware supports it, because nothing is resampled and nothing is dropped.

### Porting checklist
To reproduce this in another emulator/game:
1. You need a video backend that lets you **disable vsync / present-blocking** per-frame (e.g. `glSwapInterval(0)`, `IDXGISwapChain::Present(0, ...)`, `VK_PRESENT_MODE_IMMEDIATE_KHR`, SDL `SDL_GL_SetSwapInterval(0)`).
2. You need an audio backend that supports **blocking writes** (WASAPI shared/exclusive, ALSA blocking, XAudio2 with a bounded queue you spin on, etc.) and you must keep the latency low — bsnes recommends 40ms.
3. The main loop becomes: run one emulated frame → push audio samples (blocks if queue full, this is the throttle) → present video frame (returns immediately).
4. Recommend exclusive/borderless-with-VRR full-screen to the user; on Windows the OS/driver still needs VRR enabled in the control panel — the emulator can't force this.
5. Don't run any rate-control or audio resampling on top.

The whole feature is ~50 lines of UI glue in [_drivers.cpp:163-211](bsnes-mt/bsnes/target-bsnes/settings/_drivers.cpp#L163-L211); the real "implementation" is the discipline of using audio as the timing source while letting the GPU push frames freely. That's the entire trick worth porting.
