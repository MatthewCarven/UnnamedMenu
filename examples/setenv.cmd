@echo off
rem ---------------------------------------------------------------------
rem  The environment-variable case, which is the one that needs the trick.
rem
rem  `um` cannot set a variable in YOUR shell by running `set` -- a child
rem  process cannot touch its parent's environment. So -e makes um print the
rem  chosen command instead of running it, and the for /f loop hands that text
rem  back to this shell to execute.
rem
rem  The menu still draws on screen while this happens: um writes its interface
rem  to the console device, not to stdout, so the pipe carries only the answer.
rem ---------------------------------------------------------------------
setlocal enabledelayedexpansion

echo Pick a target environment.
echo.

rem  "delims=" matters. Without it for /f splits on spaces and %%c would be
rem  just "set" instead of the whole command.
for /f "delims=" %%c in ('um -t "Target environment" -d 1 ^
    "Development, Staging, Production : set ENV=dev, set ENV=stage, set ENV=prod"') do %%c

if not defined ENV (
    echo.
    echo   Cancelled -- nothing was set.
    exit /b 0
)

echo.
echo   ENV is now "%ENV%" in this shell.
echo.

rem  ...and it stays set for everything that follows.
if "%ENV%"=="prod" echo   Be careful.

endlocal & set ENV=%ENV%
