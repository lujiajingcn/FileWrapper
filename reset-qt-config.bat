@echo off
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ============================================================
echo  Reset Qt Creator project config (drop hardcoded paths)
echo ============================================================
echo  Project dir: %CD%
echo.

rem ---- 1) Qt Creator must be closed first ----
tasklist /FI "IMAGENAME eq qtcreator.exe" 2>nul | find /I "qtcreator.exe" >nul
if not errorlevel 1 (
    echo [ABORT] Qt Creator is still running.
    echo         Close it completely, then run this script again.
    echo.
    pause
    exit /b 1
)

rem ---- 2) Back up and remove .pro.user ----
rem     .pro.user stores an ABSOLUTE build directory, so it breaks as soon
rem     as the project moves to another drive/path. Qt Creator regenerates
rem     it from the RELATIVE default-build-directory template.
set FOUND=0
if not exist "_qt_config_backup" mkdir "_qt_config_backup"
for %%f in (FileWrapper.pro.user*) do (
    if exist "%%f" (
        echo   backup + remove: %%f
        move /Y "%%f" "_qt_config_backup\%%f" >nul
        set FOUND=1
    )
)
if "!FOUND!"=="0" echo   (no .pro.user file found, nothing to do)

rem ---- 3) Delete generated Makefiles inside build-* dirs ----
rem     Why: the command syntax inside a generated Makefile depends on the
rem     environment that ran qmake.
rem       - qmake run where sh.exe is on PATH (Git Bash / MSYS)  -> rm -f,
rem         mkdir -p, test -d, cp -f   (Unix style)
rem       - qmake run with a plain Windows PATH                  -> del,
rem         mkdir, if not exist, copy /y   (cmd style)
rem     Qt Creator drives make with cmd.exe, so a Unix-style Makefile fails
rem     with:  [Makefile:43: sub-FileWrapper-make_first] Error 1
rem     Deleting the Makefiles forces a clean regeneration with cmd syntax.
rem     Object files (.o) are kept, so the next build stays incremental.
echo.
set DELN=0
for /d %%d in (build build-* ..\build-*) do (
    if exist "%%d" (
        echo   scan: %%d
        for /f "delims=" %%m in ('dir /b /s "%%d\Makefile*" 2^>nul') do (
            del /f /q "%%m" >nul 2>&1
            set /a DELN+=1
        )
        for /f "delims=" %%s in ('dir /b /s "%%d\.qmake.stash" 2^>nul') do (
            del /f /q "%%s" >nul 2>&1
        )
    )
)
if "!DELN!"=="0" echo   [WARN] no generated Makefile found under build/ or build-*/ or ..\build-*/
echo   deleted !DELN! generated Makefile^(s^) plus .qmake.stash

echo.
echo Done. Next steps:
echo   1. Open  %CD%\FileWrapper.pro  in Qt Creator.
echo   2. Qt Creator re-runs qmake + make and rebuilds the build directory
echo      from "Tools - Options - Build ^& Run - Default build directory",
echo      which is a RELATIVE template (../build-...). The build dir
echo      therefore follows wherever this project lives.
echo.
echo NOTE: never run qmake for this project from Git Bash / MSYS by hand,
echo       or the Makefiles will be regenerated with Unix-style commands
echo       and Qt Creator will fail to build them.
echo.
pause
