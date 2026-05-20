@echo off
setlocal
set "ROOT=%~dp0"

call "%ROOT%build.bat" %*
if errorlevel 1 exit /b 1

echo [run] starting laggueless.exe
"%ROOT%build\laggueless.exe"
exit /b %errorlevel%
