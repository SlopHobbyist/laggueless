---
name: project_laggueless
description: laggueless project context — custom Win32 libretro frontend, plan-driven, currently in compat pass between steps 7 and 8.
metadata:
  type: project
---

Custom Win32 libretro frontend in C at c:\Users\stewie\Downloads\laggueless. Built with mingw gcc via build.bat. Plan-driven from plan.md with strict step gating ("STOP after each step"). Project was renamed from "multi-emulator" to "laggueless" on 2026-05-20.

**Current status (2026-05-20):** step 7 (D3D11 flip-model present with ALLOW_TEARING/SyncInterval=0) implemented and working with snes9x_libretro. Stopped before step 8 (game-picker menu) to do a compatibility pass after the user found other cores broken:
- mesen_libretro.dll and mesen-s_libretro.dll exit before `[av]` line — retro_load_game silently failing.
- nestopia_libretro.dll runs but has visible image stutter with audio enabled; perfect with --no-audio. Pace log shows retro_run cadence is fine (avg 16.7ms, no bursts, no timeouts) — issue is jitter in audio-ring-throttled wait loop releasing frames irregularly. snes9x masks it because its 60.0988 fps already beats against ~60Hz monitor.

**How to apply:** When working on this project, expect step-gated work — finish current scope, wait for user "next"/"step N" before continuing. Avoid scope creep. The compat pass is ad-hoc, not a numbered step.
