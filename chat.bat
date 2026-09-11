@echo off
setlocal
cd /d "%~dp0"
rem DarkChat application executable. Leaves build\darkui.exe (the toolkit
rem showcase) untouched. MinGW-w64 / w64devkit; no third-party dependencies.
if not exist build mkdir build
windres app.rc -O coff -o build\chat_app.res
if errorlevel 1 exit /b 1
gcc -std=c17 -municode -mwindows -Wall -Wextra -Wpedantic -Werror -O2 chat_main.c chat\chat.c chat\chat_ui.c chat\chat_host_win32.c chat\rich_text_win32.c chat\json.c chat\sse.c chat\openrouter_winhttp.c ui\ui.c ui\theme.c ui\paint.c platform\renderer.c platform\accessibility.c build\chat_app.res -o build\darkchat.exe -ld2d1 -ldwrite -ldwmapi -luiautomationcore -loleaut32 -lole32 -lgdi32 -lshell32 -lwinhttp
if errorlevel 1 exit /b 1
echo Built build\darkchat.exe
if /i "%~1"=="run" start "" "build\darkchat.exe"
if /i "%~1"=="test" (
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_sse.c chat\sse.c -o build\test_sse.exe
    if errorlevel 1 exit /b 1
    build\test_sse.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_json.c chat\json.c -o build\test_json.exe
    if errorlevel 1 exit /b 1
    build\test_json.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat.c chat\chat.c -o build\test_chat.exe
    if errorlevel 1 exit /b 1
    build\test_chat.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat_ui.c chat\chat.c chat\chat_ui.c ui\ui.c ui\theme.c -o build\test_chat_ui.exe
    if errorlevel 1 exit /b 1
    build\test_chat_ui.exe
    if errorlevel 1 exit /b 1
    echo Chat tests passed
)
exit /b 0
