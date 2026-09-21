@echo off
setlocal
cd /d "%~dp0"
rem DarkChat application executable. Building the app leaves build\darkui.exe
rem (the toolkit showcase) untouched; `chat.bat test` additionally runs the
rem toolkit suite, which rebuilds the showcase, so close it first.
rem MinGW-w64 / w64devkit; no third-party dependencies.
if not exist build mkdir build
set "progress_log=build\chat.log"
set "progress_stop=%CD%\build\.chat-progress-%RANDOM%-%RANDOM%.done"
set "DARKUI_PROGRESS_STOP=%progress_stop%"
break > "%progress_log%"
<nul set /p "=  Building DarkChat  "
start "" /b powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$f='.oOo';$i=1;$s=$env:DARKUI_PROGRESS_STOP;if(-not(Test-Path -LiteralPath $s)){[Console]::Write('.');while(-not(Test-Path -LiteralPath $s)){Start-Sleep -Milliseconds 160;[Console]::Write(([char]8).ToString()+$f[$i%%4]);$i++};[Console]::Write(([char]8).ToString()+' ')};Remove-Item -LiteralPath $s -Force"

windres app.rc -O coff -o build\chat_app.res >> "%progress_log%" 2>&1
if errorlevel 1 goto failed
gcc -std=c17 -I. -municode -mwindows -Wall -Wextra -Wpedantic -Werror -O2 chat_main.c chat\core\chat.c chat\generation\context.c chat\generation\provider_routing.c chat\core\search.c chat\shell\chat_ui.c chat\transcript\transcript_policy.c chat\transcript\transcript_win32.c chat\shell\chat_host_win32.c chat\persistence\storage.c chat\persistence\saver.c chat\shell\actions_win32.c chat\core\commands.c chat\shell\palette.c chat\shell\palette_win32.c chat\transcript\rich_text_win32.c chat\transcript\markdown.c chat\transcript\table_layout.c chat\json.c chat\export\export.c chat\import\import.c chat\generation\sse.c chat\generation\completion_request.c chat\generation\completion_winhttp.c chat\models\model_catalog.c chat\models\model_catalog_winhttp.c ui\ui.c ui\theme.c ui\paint.c platform\renderer.c platform\accessibility.c build\chat_app.res -o build\darkchat.exe -ld2d1 -ldwrite -ldwmapi -luiautomationcore -loleaut32 -lole32 -lgdi32 -lshell32 -lwinhttp -ladvapi32 -lcomdlg32 >> "%progress_log%" 2>&1
if errorlevel 1 goto failed
call :stop_progress
echo done - chat build passed
if /i "%~1"=="run" start "" "build\darkchat.exe"
if /i "%~1"=="test" (
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_sse.c chat\generation\sse.c -o build\test_sse.exe
    if errorlevel 1 exit /b 1
    build\test_sse.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_json.c chat\json.c -o build\test_json.exe
    if errorlevel 1 exit /b 1
    build\test_json.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_export.c chat\export\export.c chat\core\chat.c chat\json.c -o build\test_export.exe -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free
    if errorlevel 1 exit /b 1
    build\test_export.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_import.c chat\import\import.c chat\export\export.c chat\core\chat.c chat\json.c -o build\test_import.exe -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free
    if errorlevel 1 exit /b 1
    build\test_import.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat.c chat\core\chat.c -o build\test_chat.exe -Wl,--wrap=realloc -Wl,--wrap=malloc
    if errorlevel 1 exit /b 1
    build\test_chat.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_markdown.c chat\transcript\markdown.c -o build\test_markdown.exe
    if errorlevel 1 exit /b 1
    build\test_markdown.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_table_layout.c chat\transcript\table_layout.c -o build\test_table_layout.exe
    if errorlevel 1 exit /b 1
    build\test_table_layout.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_markdown_win.c chat\transcript\markdown.c chat\transcript\rich_text_win32.c chat\transcript\table_layout.c -o build\test_markdown_win.exe -lgdi32 -lshell32 -luser32
    if errorlevel 1 exit /b 1
    build\test_markdown_win.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_richedit_tabs.c -o build\test_richedit_tabs.exe -lgdi32 -luser32
    if errorlevel 1 exit /b 1
    build\test_richedit_tabs.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_transcript_slots.c chat\transcript\transcript_win32.c chat\transcript\transcript_policy.c chat\transcript\rich_text_win32.c chat\core\chat.c chat\transcript\markdown.c chat\transcript\table_layout.c chat\json.c -o build\test_transcript_slots.exe -lgdi32 -lshell32 -luser32 -Wl,--wrap=calloc
    if errorlevel 1 exit /b 1
    build\test_transcript_slots.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat_ui.c chat\core\chat.c chat\shell\chat_ui.c ui\ui.c ui\theme.c ui\paint.c -o build\test_chat_ui.exe
    if errorlevel 1 exit /b 1
    build\test_chat_ui.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_lifecycle.c chat\core\chat.c -o build\test_lifecycle.exe
    if errorlevel 1 exit /b 1
    build\test_lifecycle.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_context.c chat\generation\context.c chat\generation\provider_routing.c chat\generation\completion_request.c chat\core\chat.c chat\json.c -o build\test_context.exe
    if errorlevel 1 exit /b 1
    build\test_context.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_transcript_policy.c chat\transcript\transcript_policy.c -o build\test_transcript_policy.exe
    if errorlevel 1 exit /b 1
    build\test_transcript_policy.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_commands.c chat\core\commands.c chat\core\chat.c -o build\test_commands.exe
    if errorlevel 1 exit /b 1
    build\test_commands.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_palette.c chat\core\commands.c chat\core\chat.c chat\models\model_catalog.c chat\json.c -o build\test_palette.exe -Wl,--wrap=malloc -Wl,--wrap=realloc
    if errorlevel 1 exit /b 1
    build\test_palette.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_search.c chat\core\search.c chat\core\chat.c -o build\test_search.exe -Wl,--wrap=malloc -Wl,--wrap=realloc
    if errorlevel 1 exit /b 1
    build\test_search.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_storage.c chat\persistence\storage.c chat\core\chat.c chat\json.c -o build\test_storage.exe -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=calloc -Wl,--wrap=free
    if errorlevel 1 exit /b 1
    build\test_storage.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_openrouter.c chat\generation\context.c chat\generation\provider_routing.c chat\generation\completion_request.c chat\core\chat.c chat\json.c chat\generation\sse.c -o build\test_openrouter.exe -lwinhttp
    if errorlevel 1 exit /b 1
    build\test_openrouter.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_model_catalog.c chat\json.c -o build\test_model_catalog.exe -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free
    if errorlevel 1 exit /b 1
    build\test_model_catalog.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_model_catalog_worker.c chat\models\model_catalog.c chat\core\chat.c chat\json.c -o build\test_model_catalog_worker.exe -Wl,--wrap=malloc -lwinhttp
    if errorlevel 1 exit /b 1
    build\test_model_catalog_worker.exe
    if errorlevel 1 exit /b 1
    gcc -std=c17 -I. -Wall -Wextra -Wpedantic -Werror -O0 -g tests\test_chat_host.c chat\generation\context.c chat\generation\provider_routing.c chat\core\search.c chat\core\chat.c chat\shell\chat_ui.c chat\transcript\transcript_policy.c chat\transcript\transcript_win32.c chat\persistence\storage.c chat\persistence\saver.c chat\shell\actions_win32.c chat\transcript\rich_text_win32.c chat\transcript\markdown.c chat\transcript\table_layout.c chat\json.c chat\export\export.c chat\import\import.c chat\generation\sse.c chat\generation\completion_request.c chat\generation\completion_winhttp.c chat\models\model_catalog.c chat\models\model_catalog_winhttp.c chat\shell\palette.c chat\shell\palette_win32.c ui\ui.c ui\theme.c ui\paint.c platform\renderer.c platform\accessibility.c chat\core\commands.c -o build\test_chat_host.exe -Wl,--wrap=completion_request -Wl,--wrap=model_catalog_request -Wl,--wrap=palette_popup_pump -Wl,--wrap=storage_save -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=rich_text_create_block -Wl,--wrap=rich_text_create_viewport -Wl,--wrap=chat_copy_text -Wl,--wrap=chat_edit_dialog -Wl,--wrap=chat_save_dialog -Wl,--wrap=chat_open_dialog -Wl,--wrap=chat_read_file_utf8 -Wl,--wrap=chat_write_file_utf8 -Wl,--wrap=chat_export_timestamp -ld2d1 -ldwrite -ldwmapi -luiautomationcore -loleaut32 -lole32 -lgdi32 -lshell32 -lwinhttp -lcomdlg32
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

:failed
set "progress_error=%errorlevel%"
call :stop_progress
echo failed
type "%progress_log%"
exit /b %progress_error%

:stop_progress
break > "%progress_stop%"
powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$s=$env:DARKUI_PROGRESS_STOP;$limit=(Get-Date).AddSeconds(3);while((Test-Path -LiteralPath $s)-and(Get-Date)-lt$limit){Start-Sleep -Milliseconds 20};Remove-Item -LiteralPath $s -Force -ErrorAction SilentlyContinue" >nul 2>&1
exit /b 0
