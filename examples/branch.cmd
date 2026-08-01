@echo off
rem ---------------------------------------------------------------------
rem  Branching on the selection instead of eval-ing a command.
rem
rem  -x makes um exit with the 1-based index of whatever was chosen, so the
rem  menu entries can be pure labels and all the logic stays here in the batch
rem  file. Cancelling gives 0.
rem
rem  Remember that `if errorlevel N` means "N or greater", so the tests have to
rem  count DOWNWARDS or they all match the first one.
rem ---------------------------------------------------------------------

um -x -t "What would you like to do?" -d 4 ^
   "Build, Run tests, Deploy, Nothing : rem, rem, rem, rem"

set RC=%errorlevel%

if "%RC%"=="0" (
    echo Cancelled.
    exit /b 0
)

if %RC% GEQ 4 goto nothing
if %RC% GEQ 3 goto deploy
if %RC% GEQ 2 goto tests
if %RC% GEQ 1 goto build

:build
echo Building...
goto :eof

:tests
echo Running tests...
goto :eof

:deploy
rem  A destructive step gets its own confirmation, with the SAFE answer as the
rem  default. -1 lets a single keypress answer it.
rem  Every option goes BEFORE the spec -- option scanning stops at the first
rem  token that is not an option, and everything from there on is the menu.
um -x -1 -d 2 -t "Deploy to production?" "Yes, No : rem, rem"
if errorlevel 2 (
    echo Deploy cancelled.
    goto :eof
)
echo Deploying...
goto :eof

:nothing
echo Nothing to do.
goto :eof
