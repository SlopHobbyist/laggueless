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
set "OUT=%BUILD%\multi-emulator.exe"

if not exist "%BUILD%" mkdir "%BUILD%"

REM ---- flags -----------------------------------------------------------------
set "CFLAGS=-std=c11 -O2 -g -Wall -Wextra -Wshadow -Wno-unused-parameter -I%SRC% -I%ROOT%include"
set "LDFLAGS=-lgdi32 -luser32 -lopengl32 -lkernel32 -lole32 -luuid -lwinmm"

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

echo [build] OK.
exit /b 0
