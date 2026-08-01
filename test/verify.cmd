@echo off
rem ---------------------------------------------------------------------
rem  verify.cmd -- run the documented examples through a real cmd.exe.
rem
rem  Testing through anything else is not worth much: cmd tokenises the line
rem  before um ever starts, and a wrapper that builds argv itself quietly
rem  rewrites it and proves nothing.
rem
rem  Two traps this file has already fallen into, recorded so it does not
rem  happen again:
rem
rem    * `call :sub A, B, C : a, b` does NOT pass that through. Batch treats
rem      commas as argument separators, so the subroutine reassembles it
rem      space-separated and the commas are gone. Unquoted specs therefore
rem      have to be tested inline, at the top level -- which is section 1.
rem
rem    * `for /f ... in ('"%UM%" ...')` chokes when the program path is
rem      quoted. Use usebackq with backticks, and leave the path unquoted.
rem
rem    * `call :sub "a ""b"" c"` doubles the quotes on the way in and again on
rem      the way out. Anything with quotes in it is tested inline too.
rem
rem  Selections are made with -d N -w 1 so the timeout picks: that works both
rem  on the interactive path and on the no-console fallback.
rem
rem  Usage:  test\verify.cmd [path\to\um.exe]
rem ---------------------------------------------------------------------
setlocal

set UM=%~1
if "%UM%"=="" set UM=dist\um.exe
if not exist "%UM%" (
    echo Cannot find %UM% -- build first, or pass the path as an argument.
    exit /b 1
)

set PASS=0
set FAIL=0

echo.
echo   Verifying %UM%
echo.
echo   -- 1. unquoted specs, typed exactly as documented --

rem  These must be inline. See the note above about `call` and commas.
%UM% --dump Item_1, Item_2, Item_3:item_1.cmd, item_2.exe, set FOO=0 >nul 2>&1
call :judge %errorlevel% 0 "the original unquoted example"

%UM% --dump Install, Repair, Quit : setup.exe, repair.cmd, >nul 2>&1
call :judge %errorlevel% 0 "three items, one with an empty command"

%UM% --dump Recover, Cancel : C:\Windows\System32\diskpart.exe /s X:\wipe.txt, exit >nul 2>&1
call :judge %errorlevel% 0 "drive-letter colons in commands"

%UM% --dump Step 1:: Wipe, Step 2:: Restore : wipe.cmd, restore.cmd >nul 2>&1
call :judge %errorlevel% 0 "double colon is a literal colon in a name"

%UM% --dump Sort A,,Z, Cancel : "dir /o:n", exit >nul 2>&1
call :judge %errorlevel% 0 "doubled comma is a literal comma"

%UM% --dump A, B, C >nul 2>&1
call :judge %errorlevel% 255 "no colon is rejected"

%UM% --dump A, B, C : a.cmd, b.cmd >nul 2>&1
call :judge %errorlevel% 255 "count mismatch is rejected"

echo.
echo   -- 2. quoted specs (also inline: `call` would double the quotes) --

%UM% --dump "Dev, Live : set ENV=dev, set ENV=live" >nul 2>&1
call :judge %errorlevel% 0 "whole spec wrapped in one pair of quotes"

%UM% --dump Sort : "dir /o:n, then pause" >nul 2>&1
call :judge %errorlevel% 0 "quoted comma inside a command"

%UM% --dump Open : notepad "C:\Users\Admin\My Notes, draft.txt" >nul 2>&1
call :judge %errorlevel% 0 "mid-field quotes are kept and protect the comma"

%UM% --dump "Wipe, then restore", Cancel : go.cmd, exit >nul 2>&1
call :judge %errorlevel% 0 "quoted FIRST field is not mistaken for a wrapped spec"

%UM% --dump A, "B : a.cmd, b.cmd >nul 2>&1
call :judge %errorlevel% 255 "unbalanced quote is rejected"

echo.
echo   -- 3. selection and exit codes --

call :rc 2   "-e -d 2 -w 1" "A, B, C : set X=1, set X=2, set X=3"  "emit picks item 2"
call :rc 3   "-n -d 3 -w 1" "A, B, C : a, b, c"                    "name picks item 3"
call :rc 17  "-d 1 -w 1"    "Fail17, Ok : exit /b 17, exit /b 0"   "run passes the child's code"
call :rc 1   "-x -d 1 -w 1" "Fail17, Ok : exit /b 17, exit /b 0"   "-x returns the index"
call :rc 2   "-d 2 -w 1"    "Install, Quit : setup.exe,"           "empty command just selects"
call :rc 0   "-n -w 1"      "A, B : a, b"                          "timeout with no default cancels"
call :rc 255 "-d 9 -w 1"    "A, B : a, b"                          "-d out of range is rejected"
call :rc 10  "-n -d 10 -w 1" "A,B,C,D,E,F,G,H,I,J : 1,2,3,4,5,6,7,8,9,10"  "tenth item reachable"

echo.
echo   -- 4. the headline claim: -e sets a variable in THIS shell --

rem  First check the SHELL can do this at all, with um out of the picture.
rem  Wine's cmd cannot execute an internal command from a for-variable, so
rem  without this probe the next test would blame um for the shell's gap.
set FOO=
for /f "delims=" %%c in ('echo set FOO=bar') do %%c
if not "%FOO%"=="bar" (
    echo      SKIP  this shell cannot eval an internal command from for /f
    echo            ^(works on real cmd.exe; Wine's cmd does not implement it^)
    goto :after_eval
)

set ENV=
for /f "usebackq delims=" %%c in (`%UM% -e -d 2 -w 1 "Development, Staging : set ENV=dev, set ENV=stage"`) do %%c
if "%ENV%"=="stage" (call :judge 0 0 "ENV really is set to 'stage' in the calling shell") else (call :judge 1 0 "ENV is '%ENV%', expected 'stage'")

set CAP=
for /f "usebackq delims=" %%c in (`%UM% -n -d 1 -w 1 "OnlyThis, NotThis : a, b"`) do set CAP=%%c
if "%CAP%"=="OnlyThis" (call :judge 0 0 "stdout carried the answer alone, not the menu") else (call :judge 1 0 "captured '%CAP%', expected 'OnlyThis'")

:after_eval

echo.
echo   -- 5. menu file --
call :rc 3 "-f examples\recovery-menu.txt -n -d 3 -w 1" "" "menu file: separator does not consume an index"

echo.
echo   %PASS% passed, %FAIL% failed
echo.
if %FAIL% GTR 0 exit /b 1
exit /b 0

rem ----------------------------------------------------------------------
rem  :judge  GOT  WANT  "description"
:judge
if "%~1"=="%~2" (
    set /a PASS+=1
    echo      ok    %~3
) else (
    set /a FAIL+=1
    echo      FAIL  %~3   ^(exit %~1, expected %~2^)
)
goto :eof

rem  :rc  WANT  "options"  "spec"  "description"
:rc
"%UM%" %~2 "%~3" >nul 2>&1
call :judge %errorlevel% %~1 "%~4"
goto :eof
