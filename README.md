# um — a command-line menu that takes everything from the command line

`um` puts a menu on screen, lets someone pick an entry with the arrow keys or
a hotkey, and runs the command attached to it. The whole menu is described in
one line:

```
um Item_1, Item_2, Item_3 : item_1.cmd, item_2.exe, set FOO=0
```

Names before the colon, commands after it, paired up in order. No config file,
no `echo`-and-`choice` scaffolding, no writing a parser for the user's answer
every time you want a fork in the road.

```
   1) Restore system image
 > 2) Partition disk 0
   ------------------------
   3) Command prompt
   4) Reboot

   Up/Down move   Enter select   Esc cancel
   auto-selecting in 12s -- press any key to stop
```

* Single `um.exe`, about 60 KB, no installer, no runtime to deploy.
* Only imports `KERNEL32.dll` and `msvcrt.dll`, so it runs in **WinPE** as-is.
* Draws to the console device rather than stdout, so it still works when its
  output is being captured — which is what makes `set` in a menu entry
  possible (see [Setting variables](#setting-variables-in-the-calling-shell)).

---

## Build

**MSVC** — from a Developer Command Prompt:

```
build
```

**MinGW-w64** — native or cross-compiling from Linux:

```
make            # dist/um.exe
make cross      # dist/um.exe (x64) and dist/um-x86.exe (x86)
make test       # 54 parser tests, run on the build host
```

And on Windows, the end-to-end suite — 21 checks driven through a real
`cmd.exe`, which is the only way to test this honestly, since cmd tokenises the
line before `um` ever sees it:

```
test\verify.cmd
```

Prebuilt binaries are in `dist/`. Drop `um.exe` anywhere on `PATH`.

---

## The basics

```bat
um Install, Repair, Quit : setup.exe, repair.cmd,
```

Three entries. `Quit` has an empty command, so picking it runs nothing and
just exits with code 3 — enough for the caller to know what happened.

Only the **first** colon splits names from commands, so paths and URLs after
it need no escaping:

```bat
um Recover, Cancel : C:\Windows\System32\diskpart.exe /s X:\wipe.txt, exit
```

Commas inside a command need quoting or doubling:

```bat
um Sort : "dir /o:n, then pause"
um Echo : echo one,,two
```

Full rules, including how quotes are handled: **[docs/SYNTAX.md](docs/SYNTAX.md)**.
When quoting misbehaves, ask `um` what it saw:

```bat
um --dump Item_1, Item_2 : a.cmd, b.exe
```

---

## Options

| Option | Meaning |
|---|---|
| `-t, --title TEXT` | Heading above the menu |
| `-d, --default N` | Item highlighted at startup, and picked on timeout |
| `-w, --wait SECS` | Auto-pick the default after SECS with no keypress |
| `-e, --emit` | Print the chosen **command** to stdout; run nothing |
| `-n, --name` | Print the chosen **name** to stdout; run nothing |
| `-f, --file PATH` | Read the menu from a file |
| `-x, --exit-index` | Exit with the selection index, not the command's code |
| `-1, --instant` | A hotkey selects immediately, without Enter |
| `--dump` | Show how the spec parsed, then stop |
| `-h`, `-V` | Help, version |
| `--` | Stop reading options (needed if a name starts with `-`) |

**Keys:** Up/Down move, Home/End jump to the ends, `1`–`9` then `a`–`z` jump to
an item, Enter selects, Esc or Ctrl+C cancels.

---

## Setting variables in the calling shell

This is the case the design had to bend around. A child process cannot change
its parent's environment, so `um` running `set ENV=dev` for you would
accomplish nothing — the variable would vanish the moment `um` exited.

So `-e` prints the chosen command instead of running it, and the shell runs it:

```bat
for /f "delims=" %%c in ('um -e "Dev, Live : set ENV=dev, set ENV=live"') do %%c
```

Now `set` executes in *your* shell and `%ENV%` is really set. Type it straight
at the prompt with one `%` instead of two.

The menu still appears on screen during that. `um` writes its interface to the
console device (`CONOUT$`) and keeps stdout for the answer alone, so the pipe
`for /f` is reading carries nothing but the chosen command.

If you would rather branch than eval, use the exit code:

```bat
um -x Dev, Live, Cancel : rem, rem, rem
if errorlevel 3 goto cancel
if errorlevel 2 goto live
if errorlevel 1 goto dev
```

(`if errorlevel N` means "N or higher", so test downwards.)

---

## Menu files

Once a menu is more than a few entries, put it in a file:

```ini
# recovery.txt
!title  Bespoke Recovery Drive

Restore system image : X:\scripts\restore.cmd
Partition disk 0     : diskpart /s X:\scripts\wipe.txt
------------------------------------------------
Command prompt       : cmd.exe
Reboot               : wpeutil reboot
```

```bat
um -f recovery.txt -d 3 -w 30
```

Comments with `#` or `;`, a line of dashes for a divider, `!title` for the
heading. Commas are literal in a menu file — one line is one item.

---

## Exit codes

| Code | Meaning |
|---|---|
| child's code | Run mode: whatever the command returned |
| 1..N | The 1-based index chosen (`-e`, `-n`, `-x`, or an empty command) |
| 0 | Cancelled, or timed out with no default set |
| 255 | Usage or parse error — nothing ran |

Note the overlap: a command that genuinely returns 255 is indistinguishable
from a parse error in run mode. If that matters, use `-x`.

---

## The WinPE recovery drive

The original reason this exists. **[docs/WINPE.md](docs/WINPE.md)** walks
through putting `um.exe` into a boot.wim, wiring it into `startnet.cmd`, and
driving `diskpart` + image restore from the menu — including the safety
argument for never making the destructive entry the timeout default.

---

## Documentation

* **[docs/SYNTAX.md](docs/SYNTAX.md)** — the authoritative syntax spec
* **[docs/GOTCHAS.md](docs/GOTCHAS.md)** — `%var%` expansion, `for /f`, quoting traps
* **[docs/WINPE.md](docs/WINPE.md)** — recovery-drive walkthrough
* **[WORKLOG.md](WORKLOG.md)** — what was built, decisions and why
* **[TODO.md](TODO.md)** — what is deliberately not done yet

---

## Layout

```
src/um_parse.c    spec parsing -- no Win32 calls, so it can be unit tested
src/um_win.c      console rendering, key handling, execution, file loading
src/um.c          entry point, option parsing, dispatch
test/test_parse.c 54 parser tests, run with `make test`
test/verify.cmd   21 end-to-end checks through a real cmd.exe
```
