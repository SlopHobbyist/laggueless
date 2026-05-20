@echo off
setlocal enabledelayedexpansion

REM ---- gcc location ----------------------------------------------------------
set "GCC=gcc"
where %GCC% >nul 2>nul
if errorlevel 1 (
    if exist "C:\msys64\mingw64\bin\gcc.exe" (
        set "GCC=C:\msys64\mingw64\bin\gcc.exe"
        set "PATH=C:\msys64\mingw64\bin;%PATH%"
    ) else if exist "C:\mingw64\bin\gcc.exe" (
        set "GCC=C:\mingw64\bin\gcc.exe"
        set "PATH=C:\mingw64\bin;%PATH%"
    ) else (
        echo [build] ERROR: gcc not found on PATH and no fallback location matched.
        exit /b 1
    )
)

REM ---- paths -----------------------------------------------------------------
set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "BUILD=%ROOT%build"
set "OUT=%BUILD%\laggueless.exe"

if not exist "%BUILD%" mkdir "%BUILD%"

REM ---- flags -----------------------------------------------------------------
set "CFLAGS=-std=c11 -O2 -g -Wall -Wextra -Wshadow -Wno-unused-parameter -I%SRC% -I%ROOT%include"
set "LDFLAGS=-lgdi32 -luser32 -lopengl32 -lkernel32 -lole32 -luuid -lwinmm -lavrt -ld3d11 -ldxgi -lyaml"

REM ---- Vulkan SDK (optional) -------------------------------------------------
REM If VULKAN_SDK env var is set, compile with ME_HAVE_VULKAN=1 and link
REM vulkan-1. Otherwise the --vulkan flag will be present but report unavailable.
if defined VULKAN_SDK (
    if exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
        echo [build] Vulkan SDK: %VULKAN_SDK%
        set "CFLAGS=%CFLAGS% -DME_HAVE_VULKAN=1 -I%VULKAN_SDK%\Include"
        set "LDFLAGS=%LDFLAGS% -L%VULKAN_SDK%\Lib -lvulkan-1"
    ) else (
        echo [build] VULKAN_SDK is set but vulkan.h not found at %VULKAN_SDK%\Include\vulkan\vulkan.h - building without Vulkan.
    )
) else (
    echo [build] VULKAN_SDK not set - building without Vulkan ^(--vulkan will be unavailable^).
)

REM ---- collect sources -------------------------------------------------------
set "SOURCES="
for %%F in ("%SRC%\*.c") do set "SOURCES=!SOURCES! "%%F""

echo [build] gcc: %GCC%
echo [build] out: %OUT%

%GCC% %CFLAGS% %SOURCES% -o "%OUT%" %LDFLAGS%
if errorlevel 1 (
    echo [build] FAILED.
    exit /b 1
)

REM Copy libyaml runtime alongside the exe so the user doesn't need mingw64\bin on PATH.
for %%D in ("C:\msys64\mingw64\bin" "C:\mingw64\bin") do (
    if exist "%%~D\libyaml-0-2.dll" (
        copy /Y "%%~D\libyaml-0-2.dll" "%BUILD%\" >nul
        goto :dll_done
    )
)
:dll_done

echo [build] OK.
exit /b 0
