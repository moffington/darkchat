@echo off
setlocal
cd /d "%~dp0"
rem MinGW-w64 / w64devkit. No C++ runtime or third-party dependencies.
if not exist build mkdir build
set "progress_log=build\build.log"
set "progress_stop=%CD%\build\.progress-%RANDOM%-%RANDOM%.done"
set "DARKUI_PROGRESS_STOP=%progress_stop%"
break > "%progress_log%"
<nul set /p "=  Building Dark UI  "
start "" /b powershell.exe -NoLogo -NoProfile -NonInteractive -Command "$f='.oOo';$i=1;$s=$env:DARKUI_PROGRESS_STOP;if(-not(Test-Path -LiteralPath $s)){[Console]::Write('.');while(-not(Test-Path -LiteralPath $s)){Start-Sleep -Milliseconds 160;[Console]::Write(([char]8).ToString()+$f[$i%%4]);$i++};[Console]::Write(([char]8).ToString()+' ')};Remove-Item -LiteralPath $s -Force"

windres app.rc -O coff -o build\app.res >> "%progress_log%" 2>&1
if errorlevel 1 goto failed
gcc -std=c17 -I. -municode -mwindows -Wall -Wextra -Wpedantic -Werror -O2 main.c ui\ui.c ui\theme.c ui\paint.c platform\renderer.c platform\accessibility.c platform\win32.c showcase\showcase.c build\app.res -o build\darkui.exe -ld2d1 -ldwrite -ldwmapi -luiautomationcore -loleaut32 -lole32 -lgdi32 >> "%progress_log%" 2>&1
if errorlevel 1 goto failed
if /i "%~1"=="test" (
    call test.bat >> "%progress_log%" 2>&1
    if errorlevel 1 goto failed
    set "progress_result=done - build and tests passed"
) else set "progress_result=done - build passed"

call :stop_progress
echo %progress_result%
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
