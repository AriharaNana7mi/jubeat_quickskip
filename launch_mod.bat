@echo off
rem ===========================================================================
rem  Launch jubeat with the quick-skip mod loaded through spice2x's -k hook.
rem  (injector.exe is not needed for this)
rem
rem  Usage: start your local server first, then run this file.
rem  Append your usual launch arguments (e.g. -url / -card0 / -autocard) and
rem  they are passed through unchanged, for example:
rem    "launch_mod.bat" -url http://localhost:8083/ -autocard p1
rem ===========================================================================
setlocal
set "MODDIR=%~dp0"
cd /d "%MODDIR%.."

".\spice.exe" -k "jubeat_quickskip\bin\jubeat_quickskip.dll" %*

if errorlevel 1 (
    echo.
    echo [ERROR] launch failed, exit code %errorlevel%
    echo Make sure spice.exe and the jubeat_quickskip folder are inside contents\.
    pause
)
endlocal
