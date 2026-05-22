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

REM ---- lsfg backend (C++ bridge, optional) -----------------------------------
REM If the backend static library has been built (build_lsfg_backend.bat),
REM compile the C++ bridge and link against it. Requires g++ for the .cpp file.
set "LSFG_LIB=%ROOT%build\lsfg_backend\liblsfg_backend.a"
set "LSFG_OBJ="
set "LSFG_LDFLAGS="
set "BRIDGE_SRC=%SRC%\lsfg_bridge.cpp"
set "BRIDGE_OBJ=%BUILD%\lsfg_bridge.o"
set "GPP=g++"
where g++ >nul 2>nul
if errorlevel 1 (
    if exist "C:\msys64\mingw64\bin\g++.exe" set "GPP=C:\msys64\mingw64\bin\g++.exe"
)

if exist "%LSFG_LIB%" (
    echo [build] lsfg backend: %LSFG_LIB%
    "%GPP%" -std=c++20 -O2 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DME_HAVE_LSFG=1 ^
        "-I%ROOT%src\lsfg_backend\lsfg-vk-backend\include" ^
        "-I%ROOT%src\lsfg_backend\lsfg-vk-common\include" ^
        "-I%ROOT%src\lsfg_backend\lsfg-vk-common\thirdparty\include" ^
        "-I%VULKAN_SDK%\Include" ^
        "-I%SRC%" ^
        -c "%BRIDGE_SRC%" -o "%BRIDGE_OBJ%" 2>"%BUILD%\bridge_compile.log"
    if not errorlevel 1 (
        set "LSFG_OBJ=!BRIDGE_OBJ!"
        set "CFLAGS=!CFLAGS! -DME_HAVE_LSFG=1"
        set "LSFG_LDFLAGS=-Lbuild\lsfg_backend -llsfg_backend -lstdc++ -lgcc_s"
        echo [build] lsfg_bridge.cpp compiled OK.
    ) else (
        type "%BUILD%\bridge_compile.log"
        echo [build] WARNING: lsfg_bridge.cpp failed - LSFG disabled.
    )
) else (
    echo [build] lsfg backend not built yet ^(run build_lsfg_backend.bat first^).
)


REM ---- embed settings.yaml as a C header ------------------------------------
python "%ROOT%gen_settings_default.py"
if errorlevel 1 (
    echo [build] WARNING: gen_settings_default.py failed - default settings header may be stale.
)

REM ---- collect sources -------------------------------------------------------
set "SOURCES="
for %%F in ("%SRC%\*.c") do set "SOURCES=!SOURCES! "%%F""

echo [build] gcc: %GCC%
echo [build] out: %OUT%

REM ---- compile + link -------------------------------------------------------
if not "%LSFG_OBJ%"=="" (
    %GCC% %CFLAGS% %SOURCES% "%LSFG_OBJ%" -o "%OUT%" %LDFLAGS% %LSFG_LDFLAGS%
) else (
    %GCC% %CFLAGS% %SOURCES% -o "%OUT%" %LDFLAGS%
)
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

REM Copy MSYS2 C++ runtimes if the lsfg bridge was linked (required at runtime).
if not "%LSFG_OBJ%"=="" (
    for %%D in ("C:\msys64\mingw64\bin" "C:\mingw64\bin") do (
        if exist "%%~D\libgcc_s_seh-1.dll" (
            copy /Y "%%~D\libgcc_s_seh-1.dll" "%BUILD%\" >nul
            copy /Y "%%~D\libwinpthread-1.dll" "%BUILD%\" >nul
        )
    )
    if exist "C:\msys64\mingw64\bin\libstdc++-6.dll" (
        copy /Y "C:\msys64\mingw64\bin\libstdc++-6.dll" "%BUILD%\" >nul
    ) else if exist "C:\mingw64\bin\libstdc++-6.dll" (
        copy /Y "C:\mingw64\bin\libstdc++-6.dll" "%BUILD%\" >nul
    )
)

echo [build] OK.
exit /b 0
