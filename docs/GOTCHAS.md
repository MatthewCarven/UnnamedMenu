# Gotchas

Most surprises with `um` are really surprises about `cmd.exe`. These are the
ones worth knowing before they cost you an afternoon.

---

## 1. `cmd` expands `%var%` before `um` ever runs

The original example was:

```
um Item_1, Item_2, Item_3 : item_1.cmd, item_2.exe, set %item_3%=0
```

By the time `um` starts, `cmd` has already replaced `%item_3%` with the current
value of that variable. If it is not set, `cmd` substitutes nothing and `um`
receives `set =0`, which is not a valid command.

That is almost never what you want here, because the whole point is usually to
set the variable, not to read it.

**At the prompt**, quote it or use a caret:

```bat
um Item_3 : "set item_3=0"
um Item_3 : set ^%item_3^%=0
```

**In a batch file**, double the percent signs:

```bat
um Item_1, Item_2, Item_3 : item_1.cmd, item_2.exe, set %%item_3%%=0
```

To check what actually arrived, ask:

```bat
um --dump Item_3 : set %%item_3%%=0
```

Related: if you have `setlocal enabledelayedexpansion` on, `!var!` is expanded
too, and an exclamation mark in a menu name will disappear. Double it (`!!`) or
turn delayed expansion off around the `um` call.

---

## 2. `um` running `set` does nothing useful

```bat
um Dev, Live : set ENV=dev, set ENV=live
```

This runs `set ENV=dev` inside the `cmd /c` that `um` starts. That process then
exits and takes the variable with it. Your shell is unchanged. This is a rule
of the operating system, not a limitation `um` could code around.

Use `-e` and let your own shell run the command:

```bat
for /f "delims=" %%c in ('um -e "Dev, Live : set ENV=dev, set ENV=live"') do %%c
```

One `%` instead of `%%` if you are typing at the prompt rather than in a
batch file.

---

## 3. Quoting inside `for /f`

`for /f ... in ('command')` uses **single quotes** around the command, and the
command itself may contain double quotes — which is why wrapping the whole spec
in one pair of double quotes is the comfortable form:

```bat
for /f "delims=" %%c in ('um -e "A, B : set X=1, set X=2"') do %%c
```

`um` removes that outer pair itself, so the spec inside is read exactly as if
you had typed it bare.

Two things to watch:

* **`delims=` matters.** Without it, `for /f` splits the captured line on
  spaces and `%%c` gets only the first word — `set` rather than `set X=1`.
* **A caret or percent inside the emitted command gets re-parsed** when
  `do %%c` executes it. Simple `set` commands are fine; anything with `^`, `&`
  or `|` in it is easier to put in a `.cmd` file and call that instead.

`um`'s own exit code is not visible through `for /f` — the loop reports the
`for`'s status, not the child's. If you need both the command and the index,
call `um -n` separately, or use `-x` and branch on `errorlevel`.

---

## 4. Commas belong to `um`

Commas separate fields. A command that needs one must protect it:

```bat
um Sort : "dir /o:n, then pause"       rem  quote the whole field
um Echo : echo one,,two                rem  or double the comma
```

Note that `A,,B` is one field containing `A,B`, **not** two fields with an
empty one between. For a deliberately empty field write `""`.

---

## 5. A name starting with `-` or `/` looks like an option

Option scanning stops at the first token that does not start with `-` or `/` —
but a menu name that starts with one gets eaten first. Use `--`:

```bat
um -t "Verbosity" -- -verbose, -quiet : set V=1, set V=0
```

Commands are safe, because they are always after the colon and option scanning
has long since stopped by then.

---

## 6. Exit code 255 is ambiguous in run mode

In run mode `um` returns whatever the command returned, and it also uses 255
for its own usage and parse errors. A command that legitimately exits 255 is
therefore indistinguishable from a bad menu spec.

If you need to tell them apart, use `-x`, which makes `um` return the selection
index instead and leaves the command's own code unreported.

---

## 7. Redirecting output does not hide the menu — that is intentional

```bat
um -e "A, B : a, b" > chosen.txt
```

The menu still appears. `um` writes its interface to `CONOUT$`, the console
device, not to stdout — that separation is what lets `-e` be piped at all.

The consequence: you cannot capture the *menu* itself, only the answer. If you
want a silent, non-interactive selection, give `-d` and `-w` and let the
timeout choose, or run `um` with no console attached, where it falls back to a
plain numbered prompt on stderr and reads a line from stdin.

---

## 8. With no console, `-w` does not wait

If `um` cannot open the console device (a scheduled task, a service, output
fully redirected in a non-interactive context) it falls back to printing a
numbered prompt and reading a line from stdin. In that mode, reaching
end-of-input immediately selects `-d` if one was given, or cancels. It does not
sit out the `-w` countdown first, because there is nobody who could interrupt
it.

---

## 9. `um` never expands anything itself

The command text is handed to `cmd /c` verbatim, or printed verbatim with `-e`.
`um` does not expand variables, does not glob, and does not interpret the
command in any way. Everything that happens to it is `cmd` doing its usual job
one layer further down.
