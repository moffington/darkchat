@echo off
setlocal
cd /d "%~dp0"
rem DarkChat application executable. Building the app leaves build\darkui.exe
rem (the toolkit showcase) untouched; `chat.bat test` additionally runs the
rem toolkit suite, which rebuilds the showcase, so close it first.
rem MinGW-w64 / w64devkit; no third-party dependencies.
if not exist build mkdir build
windres app.rc -O coff -o build\chat_app.res
if errorlevel 1 exit /b 1
gcc -std=c17 -municode -mwindows -Wall -Wextra -Wpedantic -Werror -O2 chat_main.c chat\chat.c chat\chat_ui.c chat\transcript_win32.c chat\chat_host_win32.c chat\storage.c chat\actions_win32.c chat\rich_text_win32.c chat\markdown.c chat\json.c chat\sse.c chat\openrouter_winhttp.c ui\ui.c ui\theme.c ui\paint.c platform\renderer.c platform\accessibility.c build\chat_app.res -o build\darkchat.exe -ld2d1 -ldwrite -ldwmapi -luiautomationcore -loleaut32 -lole32 -lgdi32 -lshell32 -lwinhttp -ladvapi32
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
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat.c chat\chat.c -o build\test_chat.exe -Wl,--wrap=realloc -Wl,--wrap=malloc
    if errorlevel 1 exit /b 1
    build\test_chat.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_markdown.c chat\markdown.c -o build\test_markdown.exe
    if errorlevel 1 exit /b 1
    build\test_markdown.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_markdown_win.c chat\markdown.c chat\rich_text_win32.c -o build\test_markdown_win.exe -lgdi32 -lshell32 -luser32
    if errorlevel 1 exit /b 1
    build\test_markdown_win.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat_ui.c chat\chat.c chat\chat_ui.c ui\ui.c ui\theme.c -o build\test_chat_ui.exe
    if errorlevel 1 exit /b 1
    build\test_chat_ui.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_lifecycle.c chat\chat.c -o build\test_lifecycle.exe
    if errorlevel 1 exit /b 1
    build\test_lifecycle.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_storage.c chat\storage.c chat\chat.c chat\json.c -o build\test_storage.exe -Wl,--wrap=malloc -Wl,--wrap=realloc
    if errorlevel 1 exit /b 1
    build\test_storage.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_openrouter.c chat\chat.c chat\json.c chat\sse.c -o build\test_openrouter.exe -lwinhttp
    if errorlevel 1 exit /b 1
    build\test_openrouter.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat_host.c chat\chat.c chat\chat_ui.c chat\transcript_win32.c chat\storage.c chat\actions_win32.c chat\rich_text_win32.c chat\markdown.c chat\json.c chat\sse.c chat\openrouter_winhttp.c ui\ui.c ui\theme.c ui\paint.c platform\renderer.c platform\accessibility.c -o build\test_chat_host.exe -ld2d1 -ldwrite -ldwmapi -luiautomationcore -loleaut32 -lole32 -lgdi32 -lshell32 -lwinhttp
    if errorlevel 1 exit /b 1
    build\test_chat_host.exe
    if errorlevel 1 exit /b 1
    rem One command covers both suites: the chat tests above and the DarkUI
    rem toolkit suite (test_ui, test_renderer, test_accessibility). build.bat
    rem test rebuilds build\darkui.exe, so close a running showcase first.
    call build.bat test
    if errorlevel 1 exit /b 1
    echo Chat and toolkit suites passed
)
exit /b 0
