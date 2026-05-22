# Building Laggueless from Source (Windows 10/11, x86-64)

This guide walks you through building `laggueless.exe` from scratch on a Windows 10 or 11 machine (AMD64 / x86-64). No prior C/C++ build experience is assumed.

---

## What you will install

1. **Git for Windows** — to download (clone) the source code.
2. **MSYS2** — provides the MinGW-w64 GCC compiler and the `libyaml` library.
3. **LunarG Vulkan SDK** *(optional)* — only needed if you want the `--vulkan` and `--lsfg` features.

That's it. No Visual Studio, no CMake, no Python.

---

## Step 1 — Install Git

1. Download Git for Windows from <https://git-scm.com/download/win>.
2. Run the installer. Accept all the defaults (just click "Next" through every screen).
3. After install, open **Command Prompt** (press <kbd>Win</kbd>, type `cmd`, press <kbd>Enter</kbd>) and run:

   ```cmd
   git --version
   ```

   You should see something like `git version 2.xx.x.windows.1`. If you do, Git is installed correctly.

---

## Step 2 — Install MSYS2 and the MinGW-w64 toolchain

MSYS2 gives us `gcc`, `g++`, `ar`, and the `libyaml` library — everything `build.bat` needs.

1. Download the MSYS2 installer from <https://www.msys2.org/>. Get the `msys2-x86_64-*.exe` file near the top of the page.
2. Run the installer. **Accept the default install path of `C:\msys64`** — the build script looks for `C:\msys64\mingw64\bin\gcc.exe` and will fail if you install it somewhere else.
3. When the installer finishes, it will offer to launch MSYS2. Let it.
4. In the MSYS2 terminal window that opens, update the package database first:

   ```bash
   pacman -Syu
   ```

   When it asks `Proceed with installation? [Y/n]`, press <kbd>Y</kbd> then <kbd>Enter</kbd>. The terminal will close itself at the end of the update — this is normal.
5. Reopen MSYS2 from the Start Menu (look for **MSYS2 MSYS**) and run the update one more time to finish:

   ```bash
   pacman -Syu
   ```

6. Now install the compiler toolchain and `libyaml`:

   ```bash
   pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-libyaml
   ```

   Press <kbd>Y</kbd> when prompted. This installs both `gcc` (C compiler) and `g++` (C++ compiler, needed for the optional LSFG backend), plus `libyaml`.
7. Close the MSYS2 terminal. You don't need it again — the build script uses the compiler directly from `C:\msys64\mingw64\bin\`.

---

## Step 3 — *(Optional)* Install the Vulkan SDK

Skip this step if you only want the basic build. You can come back later.

You need this **only if** you want:
- The `--vulkan` present path (low-latency Vulkan rendering), or
- `--lsfg` LSFG 3.1 frame generation.

1. Download the Vulkan SDK from <https://vulkan.lunarg.com/sdk/home> (the **Windows** installer).
2. Run the installer. Accept the default path (typically `C:\VulkanSDK\<version>\`).
3. The installer automatically sets the `VULKAN_SDK` environment variable. To confirm, open a **new** Command Prompt and run:

   ```cmd
   echo %VULKAN_SDK%
   ```

   You should see a path like `C:\VulkanSDK\1.3.xxx.x`. If it prints `%VULKAN_SDK%` literally, log out of Windows and back in (or reboot), then try again.

> **Note:** If `VULKAN_SDK` is not set when you run `build.bat`, the build still succeeds — you just won't be able to use `--vulkan` or `--lsfg`.

---

## Step 4 — Clone the repository

Open a regular Command Prompt (not MSYS2) and pick a folder to put the source in. For example:

```cmd
cd %USERPROFILE%\Downloads
git clone https://github.com/SlopHobbyist/laggueless.git
cd laggueless
```

> If you downloaded a ZIP instead of cloning, just unzip with 7zip/winrar, and `cd` into the extracted folder.

---

## Step 5 — Build

From the project root (the folder containing `build.bat`), run:

```cmd
build.bat
```

What you should see:

```
[build] Vulkan SDK: C:\VulkanSDK\1.3.xxx.x      (only if you installed the SDK)
[build] lsfg backend not built yet (run build_lsfg_backend.bat first).
[build] gcc: C:\msys64\mingw64\bin\gcc.exe
[build] out: ...\build\laggueless.exe
[build] OK.
```

The final binary is at [build\laggueless.exe](build/laggueless.exe). The script also copies `libyaml-0-2.dll` next to it so you don't need MSYS2 on your PATH at runtime.

### Test that it ran

```cmd
build\laggueless.exe
```

You should see the usage help printed. If you do, the build worked.

---

## Step 6 — *(Optional)* Build the LSFG backend

Only do this if you installed the Vulkan SDK in Step 3 **and** you want LSFG 3.1 frame generation.

> ⚠️ **Important: order matters.** You must run `build_lsfg_backend.bat` **first**, *then* `build.bat`. The main build only links the LSFG bridge if the static library already exists. If you run `build.bat` first, it will just say `[build] lsfg backend not built yet` and produce a binary without LSFG support — re-run both in the correct order to fix it.

```cmd
:: Step 6a — FIRST: build the LSFG backend static library
build_lsfg_backend.bat

:: Step 6b — THEN: re-run the main build so it picks up the new library
build.bat
```

After step 6a you should see `[lsfg-backend] OK - library: ...\build\lsfg_backend\liblsfg_backend.a`.
After step 6b you should see `[build] lsfg backend: ...` and `[build] lsfg_bridge.cpp compiled OK.` — that confirms LSFG was linked into `laggueless.exe`.

### Obtaining `Lossless.dll` (required at runtime for `--lsfg`)

Building the LSFG backend does **not** give you the actual frame-generation DLL. That DLL is proprietary and ships with the paid **Lossless Scaling** app on Steam. You must own a copy. We do not, and cannot, distribute it.

1. Buy and install **Lossless Scaling** on Steam: <https://store.steampowered.com/app/993090/Lossless_Scaling/>.
2. Find its install folder. The easiest way:
   - Open Steam.
   - Right-click **Lossless Scaling** in your library → **Manage** → **Browse local files**.
   - Explorer opens at the install folder (typically `C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling\`).
3. Locate `Lossless.dll` in that folder.
4. Create a new folder named `lsfg` inside your `build\` folder (so the path becomes `build\lsfg\`).
5. Copy `Lossless.dll` into `build\lsfg\Lossless.dll`.

The final layout should look like:

```
laggueless\
└── build\
    ├── laggueless.exe
    ├── libyaml-0-2.dll
    ├── libgcc_s_seh-1.dll
    ├── libstdc++-6.dll
    ├── libwinpthread-1.dll
    └── lsfg\
        └── Lossless.dll        <-- your copy from Steam
```

Then run with `--lsfg --vulkan`, for example:

```cmd
build\laggueless.exe --vulkan --lsfg example-cores\mesen_libretro.dll "path\to\rom.nes"
```

Alternatively, you can skip the `build\lsfg\` folder and point directly at the DLL wherever it lives:

```cmd
build\laggueless.exe --vulkan --lsfg --lsfg-dll="C:\Program Files (x86)\Steam\steamapps\common\Lossless Scaling\Lossless.dll" example-cores\mesen_libretro.dll "path\to\rom.nes"
```

---

## Step 7 — *(Optional)* Make a distributable release folder

If you want a clean folder you can zip up and share with a friend, run:

```cmd
release.bat
```

This creates a [release\](release/) folder containing `laggueless.exe` and every DLL it needs to run on a machine that doesn't have MSYS2 installed.

---

## Troubleshooting

**`'gcc' is not recognized` / `ERROR: gcc not found`**
You either skipped Step 2 or installed MSYS2 somewhere other than `C:\msys64`. Reinstall MSYS2 to the default location and run `pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-libyaml` inside it.

**`fatal error: yaml.h: No such file or directory`**
You forgot to install `mingw-w64-x86_64-libyaml`. Open MSYS2 and run the `pacman -S ...` command from Step 2 again.

**`cannot find -lyaml`**
Same fix — install `mingw-w64-x86_64-libyaml` via pacman.

**`VULKAN_SDK is set but vulkan.h not found`**
Your Vulkan SDK install is incomplete or the env var points at the wrong place. Reinstall the SDK from <https://vulkan.lunarg.com/sdk/home> and reboot.

**`build_lsfg_backend.bat` says `ERROR: VULKAN_SDK not set`**
You skipped Step 3, or `VULKAN_SDK` isn't visible to the Command Prompt. Open a **new** Command Prompt after installing the SDK (env vars don't propagate to already-open windows).

**`'g++' not found` when building the LSFG backend**
The `mingw-w64-x86_64-gcc` package from Step 2 includes `g++`. If it's missing, run that pacman command again — accept the package replacement if asked.

**`libyaml-0-2.dll was not found` when running `laggueless.exe`**
The build script normally copies this DLL automatically. If it didn't, copy `C:\msys64\mingw64\bin\libyaml-0-2.dll` into the `build\` folder manually.

---

## Summary cheat-sheet

```cmd
:: One-time setup
:: 1. Install Git from https://git-scm.com/download/win
:: 2. Install MSYS2 from https://www.msys2.org/ (default path C:\msys64)
::    Then, in MSYS2:
::      pacman -Syu                                          (twice)
::      pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-libyaml
:: 3. (Optional) Install Vulkan SDK from https://vulkan.lunarg.com/sdk/home

:: Build
git clone https://github.com/<owner>/laggueless.git
cd laggueless
build.bat

:: Optional LSFG support
build_lsfg_backend.bat
build.bat

:: Optional standalone release folder
release.bat
```

That's it — `build\laggueless.exe` is your binary.
