# Audio Quality Improvement Plan

## Background

Current audio implementation uses WASAPI shared-mode with a custom ring buffer and
a linear-interpolation resampler driven by per-frame DRC (dynamic rate control).

Already applied:
- WASAPI buffer reduced from 50 ms → 20 ms (`audio_wasapi.c:115`)
- Audio thread promoted to `Pro Audio` MMCSS priority (`audio_wasapi.c`)
- DRC `target_buffered` reduced from 50 ms → 20 ms (`main.c:1027`)
- `-lavrt` added to `build.bat`

Result: delay reduced, but audio still sounds "crunchy."

## Root cause (from Mesen2 comparison)

1. **Linear resampling** in `resample_and_push` (`main.c:412-429`) — the dominant
   cause of crunchy/gritty sound. Linear interpolation between input samples
   introduces audible high-frequency aliasing at typical core-rate → device-rate
   ratios (e.g. 32040 → 48000 Hz). Mesen uses 4-point cubic Hermite.

2. **Per-frame DRC reading** (`main.c:1125`) — reads instantaneous ring fill
   every frame and feeds it into rate bias. This makes the resampler ratio
   jitter, producing pitch wobble. Mesen averages cursor gap over 60 frames
   (~1 second) before adjusting rate.

3. **Rate adjustments too aggressive** — Mesen caps rate change at ±0.25% and
   steps in 0.003125% increments. Our `g_resamp_ratio_bias + g_resamp_p_bias`
   can reach ±0.65% with no smoothing.

## Steps

### Step 1 — Replace linear interpolation with 4-point cubic Hermite
**File:** `src/main.c` (`resample_and_push`, around lines 388-434)

- Keep last 3 input samples (`pp_l, pp_r`, `p_l, p_r`, `n_l, n_r`) plus the
  upcoming sample (`nn_l, nn_r`) for the 4-point kernel.
- Replace the linear lerp at lines 416-417 with cubic Hermite:
  ```
  // c0..c3 derived from 4 neighbors and fractional phase t
  float c0 = p;
  float c1 = 0.5f * (n - pp);
  float c2 = pp - 2.5f*p + 2.0f*n - 0.5f*nn;
  float c3 = 0.5f*(nn - pp) + 1.5f*(p - n);
  float out = ((c3*t + c2)*t + c1)*t + c0;
  ```
- Update saved state: `g_resamp_prev_l/r` becomes a 3-sample history.
- Preserve the existing DRC bias math — don't touch `step` calculation.
- Same chunked `me_audio_push` flow, no change to sync semantics.

**Why this preserves audio-driven sync:** cores still call into the resampler at
their natural rate; the only thing that changes is *how* we interpolate between
their samples. Output sample count per call is identical to before.

### Step 2 — Add 60-frame moving average for DRC fill measurement
**File:** `src/main.c` (around lines 1072-1130)

- Add `static size_t fill_history[60]; static int fill_idx = 0; static int fill_filled = 0;`
- Each frame, store current `fill_after` into the ring history.
- Only start computing `err` once the history is filled (60 frames ≈ 1 second).
- Use `avg_fill = sum(history) / 60` in place of instantaneous `fill` for the
  PID error calculation.
- Leave the unfiltered `fill` available for the `pace_log` printout so we can
  still see real-time behavior.

**Why:** stops the rate bias from chasing per-frame ring-fill noise; only
reacts to genuine drift between the core and device clocks.

### Step 3 — Tighten DRC adjustment ceiling
**File:** `src/main.c` (search for the clamping of `g_resamp_p_bias` / `g_resamp_ratio_bias`)

- Cap total bias magnitude at ±0.25% (currently ~0.65%).
- Reduce proportional gain so per-frame `p_bias` changes are gentler.
- Aim for: small persistent integral term handles steady-state clock offset,
  proportional term only nudges on transient ring-fill deviations.

**Why:** smaller, slower rate changes are inaudible (Mesen targets <8 cents
pitch shift); current ±0.65% (~11 cents) is at the threshold of audibility and
combines badly with the noisy per-frame measurement.

### Step 4 — Verify and iterate
Build, run each core (especially audio-sync-sensitive ones like SNES bsnes/mt
and any PCE core), and listen for:
- Crunchiness gone (Step 1 should handle this)
- No pitch wobble during steady gameplay (Step 2)
- No audible pitch glide on transient frame-time spikes (Step 3)
- Game speed still tracks correctly for cores that audio-sync (no slowdown
  or speed-up)

If a core that audio-syncs misbehaves, the suspect is Step 2/3 — bias may be
adjusting too slowly to keep up with that core's clock. Mitigation: shrink the
moving-average window from 60 → 30 frames.

## Out of scope (intentionally not changing)

- Ring buffer locking — restructuring it risks breaking sync-driven cores; the
  current `CRITICAL_SECTION` approach works.
- Switching to XAudio2 / miniaudio / SDL — would be a bigger rewrite and the
  remaining issues are interpolation + control-loop quality, not API choice.
- WASAPI exclusive mode — would lower latency further but locks out other
  apps; not worth it given 20 ms shared-mode is already low.
