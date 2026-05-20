---
name: project_libyaml_dependency
description: multi-emulator now depends on libyaml (mingw-w64-x86_64-libyaml). build.bat links -lyaml and stages libyaml-0-2.dll into build/.
metadata:
  type: project
---

multi-emulator added a libyaml dependency on 2026-05-20 to parse settings.yaml. Package `mingw-w64-x86_64-libyaml` (0.2.5-2) installed via pacman in the user's msys64 environment.

**Why:** [[project_multi_emulator]] needed a settings.yaml loader; user picked libyaml over hand-written parser when given the options.

**How to apply:** When working on this project on a different machine, libyaml must be installed (`pacman -S mingw-w64-x86_64-libyaml`). build.bat links with `-lyaml` and copies `libyaml-0-2.dll` from `C:\msys64\mingw64\bin` next to the exe automatically. If port shims (step 10) target Linux later, `libyaml-dev`/`libyaml-devel` is the equivalent package.
