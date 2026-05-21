@echo off
setlocal enabledelayedexpansion

REM ---- build_lsfg_backend.bat
REM Compiles the Win32-ported lsfg-vk backend as a static library (.a)
REM and links a minimal test harness that creates an Instance, parses Lossless.dll,
REM and verifies shader extraction. No real frames are generated.
REM
REM Requires: MSYS2 g++ (C++20) and the Vulkan SDK.

REM ---- locate g++ -------------------------------------------------------
set "GPP=g++"
where %GPP% >nul 2>nul
if errorlevel 1 (
    if exist "C:\msys64\mingw64\bin\g++.exe" (
        set "GPP=C:\msys64\mingw64\bin\g++.exe"
        set "PATH=C:\msys64\mingw64\bin;%PATH%"
    ) else (
        echo [lsfg-backend] ERROR: g++ not found.
        exit /b 1
    )
)

set "AR=ar"
where %AR% >nul 2>nul
if errorlevel 1 set "AR=C:\msys64\mingw64\bin\ar.exe"

REM ---- paths ------------------------------------------------------------
set "ROOT=%~dp0"
set "BACKEND=%ROOT%src\lsfg_backend"
set "COMMON=%BACKEND%\lsfg-vk-common"
set "BACKLIB=%BACKEND%\lsfg-vk-backend"
set "BUILD=%ROOT%build\lsfg_backend"
set "LIB=%BUILD%\liblsfg_backend.a"

if not defined VULKAN_SDK (
    echo [lsfg-backend] ERROR: VULKAN_SDK not set.
    exit /b 1
)
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" (
    echo [lsfg-backend] ERROR: vulkan.h not found at %VULKAN_SDK%\Include\vulkan\vulkan.h
    exit /b 1
)

if not exist "%BUILD%" mkdir "%BUILD%"

REM ---- include paths ----------------------------------------------------
set "INC=-I"%BACKEND%\lsfg-vk-backend\include" -I"%COMMON%\include" -I"%COMMON%\thirdparty\include" -I"%VULKAN_SDK%\Include""

REM ---- compiler flags ---------------------------------------------------
set "CXXFLAGS=-std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -DWIN32_LEAN_AND_MEAN -DNOMINMAX"

REM ---- collect sources --------------------------------------------------
set "SOURCES="

REM common/vulkan
for %%F in ("%COMMON%\src\vulkan\*.cpp") do set "SOURCES=!SOURCES! "%%F""

REM common/helpers
for %%F in ("%COMMON%\src\helpers\*.cpp") do set "SOURCES=!SOURCES! "%%F""

REM common/configuration
for %%F in ("%COMMON%\src\configuration\*.cpp") do set "SOURCES=!SOURCES! "%%F""

REM backend extraction
for %%F in ("%BACKLIB%\src\extraction\*.cpp") do set "SOURCES=!SOURCES! "%%F""

REM backend helpers
for %%F in ("%BACKLIB%\src\helpers\*.cpp") do set "SOURCES=!SOURCES! "%%F""

REM backend shaderchains
for %%F in ("%BACKLIB%\src\shaderchains\*.cpp") do set "SOURCES=!SOURCES! "%%F""

REM backend main
set "SOURCES=!SOURCES! "%BACKLIB%\src\lsfgvk.cpp""

REM ---- compile each source to an object file ----------------------------
echo [lsfg-backend] Compiling %SOURCES: =    %...
set "OBJECTS="
set "FAIL=0"
for %%F in (%SOURCES%) do (
    set "SRC=%%~F"
    set "OBJ=%BUILD%\%%~nF.o"
    set "OBJECTS=!OBJECTS! "!OBJ!""
    "%GPP%" %CXXFLAGS% %INC% -c "%%~F" -o "!OBJ!"
    if errorlevel 1 (
        echo [lsfg-backend] FAILED: %%~F
        set "FAIL=1"
    )
)
if "%FAIL%"=="1" (
    echo [lsfg-backend] Build FAILED.
    exit /b 1
)

REM ---- archive into static lib ------------------------------------------
echo [lsfg-backend] Archiving to %LIB%...
"%AR%" rcs "%LIB%" %OBJECTS%
if errorlevel 1 (
    echo [lsfg-backend] Archive FAILED.
    exit /b 1
)

REM ---- compile test harness ---------------------------------------------
echo [lsfg-backend] Building test harness...
set "TEST_SRC=%ROOT%src\lsfg_backend_test.cpp"
set "TEST_EXE=%BUILD%\lsfg_backend_test.exe"

"%GPP%" %CXXFLAGS% %INC% "%TEST_SRC%" -o "%TEST_EXE%" ^
    -L"%BUILD%" -llsfg_backend ^
    -L"%VULKAN_SDK%\Lib" -lvulkan-1 ^
    -lkernel32 -luser32 -lole32 -luuid
if errorlevel 1 (
    echo [lsfg-backend] Test harness FAILED to compile.
    exit /b 1
)

echo [lsfg-backend] OK - library: %LIB%
echo [lsfg-backend] OK - test:    %TEST_EXE%
exit /b 0
