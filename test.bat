@echo off
setlocal
cd /d "%~dp0"
if not exist build mkdir build
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_ui.c ui\ui.c ui\theme.c ui\paint.c showcase\showcase.c -o build\test_ui.exe
if errorlevel 1 exit /b 1
build\test_ui.exe
if errorlevel 1 exit /b 1
gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_renderer.c ui\ui.c ui\theme.c ui\paint.c showcase\showcase.c platform\renderer.c -o build\test_renderer.exe -ld2d1 -ldwrite -lole32 -lgdi32
if errorlevel 1 exit /b 1
build\test_renderer.exe
exit /b %errorlevel%
