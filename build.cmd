@echo off
rem ---------------------------------------------------------------------
rem  build.cmd -- build um.exe with the Microsoft toolchain.
rem
rem  Run this from a "Developer Command Prompt for VS" (any edition,
rem  including Build Tools) so that cl.exe and the SDK are on PATH.
rem  Visual Studio 2015 or newer -- earlier versions ship a non-conforming
rem  swprintf that takes no buffer-size argument.
rem
rem    build            release build -> dist\um.exe
rem    build debug      unoptimised build with symbols
rem
rem  /MT statically links the CRT. Do not change that: a WinPE image has no
rem  redistributable CRT installed, so a /MD build would fail to start there
rem  with a missing-DLL error and nothing useful on screen.
rem ---------------------------------------------------------------------
setlocal

where cl >nul 2>&1
if errorlevel 1 (
    echo.
    echo   cl.exe was not found.
    echo   Open a "Developer Command Prompt for VS" and run this again,
    echo   or use the MinGW build instead:  mingw32-make
    echo.
    exit /b 1
)

if not exist dist  mkdir dist
if not exist build mkdir build

set SRC=src\um.c src\um_win.c src\um_parse.c
set OPT=/O1 /GS- /DNDEBUG
set OUT=dist\um.exe

if /i "%~1"=="debug" (
    set OPT=/Od /Zi /DDEBUG
    set OUT=dist\um-debug.exe
)

cl /nologo /W4 /MT %OPT% /Isrc /Fo:build\ /Fd:build\ /Fe:%OUT% ^
   %SRC% /link /SUBSYSTEM:CONSOLE kernel32.lib
if errorlevel 1 (
    echo.
    echo   BUILD FAILED
    exit /b 1
)

echo.
echo   Built %OUT%
for %%F in (%OUT%) do echo   %%~zF bytes
echo.
echo   Quick check:
%OUT% --dump Item_1, Item_2, Item_3:item_1.cmd, item_2.exe, set FOO=0
endlocal
