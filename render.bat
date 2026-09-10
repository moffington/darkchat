@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O2 tests\render_showcase.c ui\ui.c ui\theme.c ui\paint.c showcase\showcase.c platform\renderer.c -o build\render_showcase.exe -ld2d1 -ldwrite -lole32 -lgdi32 -lwindowscodecs
if errorlevel 1 exit /b 1
build\render_showcase.exe
exit /b %errorlevel%
