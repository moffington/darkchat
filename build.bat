@echo off
setlocal
cd /d "%~dp0"
rem MinGW-w64 / w64devkit. No C++ runtime or third-party dependencies.
if not exist build mkdir build
windres app.rc -O coff -o build\app.res
if errorlevel 1 exit /b 1
gcc -std=c17 -municode -mwindows -Wall -Wextra -Wpedantic -Werror -O2 main.c ui\ui.c ui\theme.c ui\paint.c platform\renderer.c platform\win32.c showcase\showcase.c build\app.res -o build\darkui.exe -ld2d1 -ldwrite -ldwmapi -lole32 -lgdi32
if errorlevel 1 exit /b 1
echo Build OK: build\darkui.exe
if /i "%~1"=="test" call test.bat
exit /b %errorlevel%
