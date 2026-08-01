# Worklog

## 2026-08-01 — initial build, v0.4.0

Built `um` from the brief in one session, in slices. Parser first and tested in
isolation, then the console layer, then execution, then the extras.

### Decisions and why

**Native C, single .exe.** The stated endgame is a WinPE recovery drive.
PowerShell would need the `WinPE-PowerShell` optional component added to the
image; .NET likewise. A batch implementation cannot read a single keypress or
draw a highlight without ugly hacks. C compiles to ~60 KB importing only
`KERNEL32.dll` and `msvcrt.dll`, both of which WinPE already has. Verified with
`objdump -p`.

**`um` reads its raw command line and never looks at `argv`.** This is the
decision the whole syntax rests on. The brief's example is unquoted:

```
um Item_1, Item_2, Item_3:item_1.cmd, item_2.exe, set %item_3%=0
```

The C runtime would split that on whitespace, so `set %item_3%=0` would arrive
as two separate arguments with the space between them lost. `GetCommandLineW()`
plus stepping over the program name keeps the line exactly as typed, and lets
`um` decide what the commas and colon mean. Documented in docs/SYNTAX.md §2.

**Only the first colon splits.** Means `C:\Windows\...` and `http://...` in
commands need no escaping at all, which matters a lot for the recovery-drive
case. Colons in *names* are written `::`.

**Quotes are stripped only when they wrap a whole field.** A quote appearing
mid-field is kept but still protects commas. This is the behaviour Windows
actually needs: `notepad "C:\Program Files\x.txt"` has to reach `cmd` with its
quotes intact, while `"Wipe, then restore"` as a menu name should lose them.

**The menu is drawn to `CONOUT$`, not stdout.** This is what makes `-e` usable
inside `for /f`. Under that construct cmd replaces stdout with a pipe; writing
the interface to the console device keeps the two channels separate, so the
user sees the menu and the pipe carries only the chosen command.

**Colour via `SetConsoleTextAttribute`, not ANSI/VT.** VT processing cannot be
relied on in WinPE's conhost. Reverse video is done by swapping the fg/bg
nibbles rather than `COMMON_LVB_REVERSE_VIDEO`, which older conhosts ignore.

**Hotkeys move the highlight; Enter confirms.** Deliberately not
select-on-keypress by default, because one of the menu entries in the intended
use case reformats a disk. `-1` opts into instant selection for confirmations
and other harmless menus.

**Run mode returns the child's exit code; `-x` returns the index instead.**
A launcher should be transparent by default. The overlap with 255 (used for
usage errors) is documented rather than worked around.

### Two real bugs found by testing, both fixed

1. **A fully quoted spec was rejected.** `um -e "A, B : a, b"` — the exact form
   the help text itself recommends inside `for /f` — failed with "no ':'
   separator", because the whole spec sat inside one quoted run. Added
   `um_unwrap_spec()`, which removes a single pair of quotes only when the
   quote that opens the string is the one closing it at the very end, so a spec
   whose *first field* is quoted is left alone. Covered by 8 tests.

2. **The `-w` timeout could hang forever.** The original loop did
   `WaitForSingleObject` on the console input handle then `ReadConsoleInputW`.
   A console handle signals for any input record and can report itself
   signalled while the queue is effectively empty, at which point the read
   blocks indefinitely and the countdown stops — which defeats the entire point
   of an unattended timeout. Now `GetNumberOfConsoleInputEvents` gates every
   read, the wait is only used to sleep politely and its result is not trusted,
   and time remaining comes from `GetTickCount()` rather than accumulated poll
   intervals.

While chasing (2) it became clear that *any* key event stopped the countdown,
including a bare Shift or Ctrl key-down. On an unattended recovery drive a
nudged keyboard would then leave the machine sitting at a menu forever. Added
`key_is_meaningful()` so modifier-only presses and characterless synthetic
events are ignored.

### Verification

* **54 parser tests**, run natively on the build host — `um_parse.c` has no
  Win32 calls specifically so this is possible. `make test`.
* **21 end-to-end tests through a real `cmd.exe`** (`test\verify.cmd`). This
  layer earns its keep: cmd tokenises the line before `um` starts, so a test
  runner that builds argv itself rewrites the command line and proves nothing.
  Three of the documented examples turned out to be wrong and were only caught
  here — a spec whose confirmation menu had commas inside the names (one name,
  two commands), an `-x` written after the spec where option scanning had
  already stopped, and a run-mode call reading an index that run mode does not
  return.
* End-to-end under Wine, including a pty so the real console path is exercised:
  navigation, wrapping, hotkeys (`1`-`9` then `a`-`z`), instant mode, Esc,
  Ctrl+C, countdown decrementing and firing, modifier-keys-do-not-cancel,
  emit/name/run modes, exit codes, menu files, separators not consuming an
  index, and every error path.
* Cross-compiled x64 and x86. Import table checked for WinPE suitability.

### Testing traps, written down so they are not rediscovered

* **Wine synthesises about six key events when a pty's stdin closes.** Those
  cancel the countdown, and the result looks exactly like the timeout hanging.
  Hold stdin open (`{ printf ...; sleep 6; } | script ...`) or every timeout
  test lies to you. This cost an hour and sent me looking for a bug in the
  input loop that was not there — though it did lead to the modifier-key fix,
  which is real.
* **Batch splits `call` arguments on commas.** `call :sub A, B : a, b` hands
  the subroutine five arguments and reassembling them space-separated loses
  every comma. The first version of `verify.cmd` did this and its unquoted-spec
  tests all passed for the wrong reason. Unquoted specs must be tested inline,
  at the top level.
* **`for /f ... in ('"%UM%" ...')` chokes on a quoted program path** in Wine's
  cmd. Use `usebackq` with backticks and leave the path unquoted.
* **Wine's cmd cannot execute an internal command from a for-variable.**
  `for /f "delims=" %%c in ('echo set FOO=bar') do %%c` fails there with `um`
  entirely absent, so the env-var test cannot pass under Wine. `verify.cmd` now
  probes for this and reports SKIP rather than blaming `um`. The idiom itself
  is standard and works on real `cmd.exe`; what *is* verified under Wine is
  that `um` emits exactly `set ENV=stage` with correct CRLF framing and that
  `for /f` captures it cleanly with nothing of the menu leaking into the pipe.

### Repository

`git init` run in the project folder on 2026-08-01, after one detour into the
neighbouring `TUI` folder — which now has a stray empty `.git` worth removing
if that was a slip (`Remove-Item -Recurse -Force .git`; nothing is lost, it
never had a commit). First commit made from this session.

Two things went in before that first commit:

* **`.gitattributes` pinning `.cmd` and `.txt` to CRLF.** The files had reached
  disk with Unix endings. cmd.exe is mostly tolerant of LF-only batch files but
  not reliably so around labels and `goto`, and diskpart is fussier still. A
  whitespace problem that presents as a logic bug is a bad way to spend an
  evening. C sources and Markdown stay LF so diffs stay clean when the repo is
  touched from Linux.
* **`.gitignore` that deliberately does *not* exclude `dist/`.** `um.exe` is
  60 KB with no dependencies, and the whole point of it is being copyable onto
  a USB stick or into a boot.wim at short notice — having the built binary in
  the repo means no toolchain needed at the moment you want it. One commented
  line flips that if you would rather keep binaries out.

The first commit was authored as `Matthew <matthewcarven@gmail.com>`, passed
per-command so nothing was written to any git config. If your usual identity
differs, `git commit --amend --author="Name <email>"` fixes it.

### Note: git does not work well from the Cowork device bridge

The first commit was made from this session, but only after a fight. Every git
command that writes leaves an `index.lock` / `HEAD.lock` behind in the mounted
folder and cannot unlink it (`Operation not permitted`), so each subsequent git
call refuses to start. The bridge can move files but not delete them, so the
locks have to be shuffled aside one at a time and the loose `tmp_obj_*` files
in `.git/objects` cannot be cleaned up at all.

Conclusion for next time: run git from PowerShell on the machine itself. It is
faster than working around the mount and it does not leave debris. Anything in
this repo that needs staging or amending is a five-second job there.
