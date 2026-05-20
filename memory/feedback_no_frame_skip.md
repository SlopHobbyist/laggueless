---
name: feedback_no_frame_skip
description: Hard rule for multi-emulator project — emulated frames must not be skipped or duplicated. Speedrun/competition compliance.
metadata:
  type: feedback
---

Do not propose or implement frame-skipping, frame-duplication, or any pacing scheme that drops/repeats emulator frames. Every retro_run output must reach the screen exactly once.

**Why:** the user needs this program to be compliant with common competition and speedrun rules on emulators. Skipping/duplicating frames invalidates runs and silently changes gameplay timing.

**How to apply:** when fixing pacing/sync/stutter issues, restrict yourself to changing *when* a frame is presented (timing, buffering, deferral), never *whether* it's presented. Don't suggest "skip a frame if ring is full" or "duplicate a frame to match refresh rate" — both are off-limits. Audio resampling and present-time adjustment are fine; frame dropping/duplication is not. See also [[feedback_generic_fixes]].
