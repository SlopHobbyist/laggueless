---
name: feedback_generic_fixes
description: Prefer generic fixes that help multiple libretro cores at once; avoid per-core branches.
metadata:
  type: feedback
---

When fixing compatibility or pacing issues, prefer changes that apply to the whole frontend / all cores. Avoid adding `if (core is Mesen) ...` branches or per-core special cases unless there's truly no generic alternative.

**Why:** the goal is a minimal frontend that supports many cores. Per-core hacks accumulate and become maintenance debt; they also imply the frontend is broken for cores the user hasn't tested yet.

**How to apply:** when diagnosing a single broken core, look for the underlying spec compliance gap (e.g. missing environment callback, wrong pixel format handling, audio rate assumption) and fix it generically. Only fall back to per-core handling after exhausting generic options.
