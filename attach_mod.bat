@echo off
rem ===========================================================================
rem  Optional: inject the quick-skip mod into an ALREADY RUNNING jubeat.
rem
rem  The normal way is to launch the game with spice2x's -k option
rem  (launch_mod.bat). Use this file only when the game is already running and
rem  you do not want to restart it.
rem
rem  Do NOT use this together with -k on the same process: one process can only
rem  load the mod once (the second attempt fails harmlessly and logs
rem  "ERROR unexpected input_update prologue").
rem ===========================================================================
setlocal
set "MODDIR=%~dp0"
cd /d "%MODDIR%.."

set "SPICEPID="
for /f "usebackq delims=" %%i in (`powershell -NoProfile -Command "(Get-Process spice -ErrorAction SilentlyContinue | Sort-Object StartTime | Select-Object -First 1).Id"`) do set "SPICEPID=%%i"

if "%SPICEPID%"=="" (
    echo [ERROR] spice.exe is not running. Start the game first.
    pause
    exit /b 1
)

echo Found spice.exe, PID = %SPICEPID%
"%MODDIR%bin\injector.exe" --dll "%MODDIR%bin\jubeat_quickskip.dll" -p %SPICEPID%

if errorlevel 1 (
    echo.
    echo [ERROR] injection failed, exit code %errorlevel%
    pause
    exit /b 1
)
echo Injection done. During play, hold the configured panels to quick-skip.
timeout /t 4 >nul
endlocal
