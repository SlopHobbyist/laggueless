@echo off
setlocal
set "ROOT=%~dp0"
set FAILED=

:: Step 1: build
echo [1/8] Building...
call "%ROOT%build.bat" %*
if errorlevel 1 (
    echo [FAIL] build.bat failed
    set FAILED=%FAILED% build
    goto :done
)
echo [OK] build.bat

:: Step 2: release
echo [2/8] Running release.bat...
call "%ROOT%release.bat"
if errorlevel 1 (
    echo [FAIL] release.bat failed
    set FAILED=%FAILED% release
    goto :done
)
echo [OK] release.bat

:: Step 3: Copy example-cores to release\cores
echo [3/8] Copying example-cores to release\cores...
if not exist "%ROOT%release\cores" mkdir "%ROOT%release\cores"
xcopy /e /y /q "%ROOT%example-cores\*" "%ROOT%release\cores\" >nul
if errorlevel 1 (
    echo [FAIL] Copy example-cores failed
    set FAILED=%FAILED% example-cores
) else (
    echo [OK] example-cores
)

:: Step 4: Copy example-roms to release\roms
echo [4/8] Copying example-roms to release\roms...
if not exist "%ROOT%release\roms" mkdir "%ROOT%release\roms"
xcopy /e /y /q "%ROOT%example-roms\*" "%ROOT%release\roms\" >nul
if errorlevel 1 (
    echo [FAIL] Copy example-roms failed
    set FAILED=%FAILED% example-roms
) else (
    echo [OK] example-roms
)

:: Step 5: Copy firmware to release\firmware
echo [5/8] Copying firmware to release\firmware...
if not exist "%ROOT%release\firmware" mkdir "%ROOT%release\firmware"
xcopy /e /y /q "%ROOT%firmware\*" "%ROOT%release\firmware\" >nul
if errorlevel 1 (
    echo [FAIL] Copy firmware failed
    set FAILED=%FAILED% firmware
) else (
    echo [OK] firmware
)

:: Step 6: Copy saves to release\saves
echo [6/8] Copying saves to release\saves...
if not exist "%ROOT%release\saves" mkdir "%ROOT%release\saves"
xcopy /e /y /q "%ROOT%saves\*" "%ROOT%release\saves\" >nul 2>nul
echo [OK] saves

:: Step 7: Copy Lossless.dll (optional)
echo [7/8] Copying Lossless.dll (optional)...
if exist "%ROOT%build\lsfg\Lossless.dll" (
    if not exist "%ROOT%release\lsfg" mkdir "%ROOT%release\lsfg"
    copy /y "%ROOT%build\lsfg\Lossless.dll" "%ROOT%release\lsfg\Lossless.dll" >nul
    if errorlevel 1 (
        echo [FAIL] Copy Lossless.dll failed
        set FAILED=%FAILED% Lossless.dll
    ) else (
        echo [OK] Lossless.dll
    )
) else (
    echo [SKIP] build\lsfg\Lossless.dll not found, skipping
)

:: Step 8: Run
echo [8/8] Launching laggueless.exe...
if defined FAILED (
    echo.
    echo [WARN] Some steps failed:%FAILED% -- attempting launch anyway
)
"%ROOT%release\laggueless.exe" ".\release\cores\mesen_libretro.dll" ".\release\roms\Super Mario Bros. (World).nes" --vulkan --lsfg
if errorlevel 1 (
    echo [FAIL] laggueless.exe exited with error
    set FAILED=%FAILED% launch
)

:done
if defined FAILED (
    echo.
    echo Failed steps:%FAILED%
    exit /b 1
)
exit /b 0
