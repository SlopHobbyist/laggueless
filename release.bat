@echo off
setlocal

set SRC=build
set DST=release

if not exist "%SRC%\laggueless.exe" (
    echo ERROR: %SRC%\laggueless.exe not found. Run build.bat first.
    exit /b 1
)

if exist "%DST%" rmdir /s /q "%DST%"
mkdir "%DST%"

copy /y "%SRC%\laggueless.exe"        "%DST%\" >nul || goto :fail
copy /y "%SRC%\libgcc_s_seh-1.dll"    "%DST%\" >nul || goto :fail
copy /y "%SRC%\libstdc++-6.dll"       "%DST%\" >nul || goto :fail
copy /y "%SRC%\libwinpthread-1.dll"   "%DST%\" >nul || goto :fail
copy /y "%SRC%\libyaml-0-2.dll"       "%DST%\" >nul || goto :fail

echo.
echo Release built in %DST%\
echo   (Players must supply their own Lossless.dll in %DST%\lsfg\ to use --lsfg)
exit /b 0

:fail
echo ERROR: copy failed.
exit /b 1
