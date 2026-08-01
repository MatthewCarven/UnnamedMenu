# `um` — syntax specification

This is the authoritative description of how `um` reads its command line.
The implementation in `src/um_parse.c` is written to match this document; if
they ever disagree, that is a bug in the code.

---

## 1. Overall shape

```
um [OPTIONS] <names> : <commands>
um [OPTIONS] -f <menufile>
```

The original worked example, verbatim:

```
um Item_1, Item_2, Item_3:item_1.cmd, item_2.exe, set %item_3%=0
```

Three menu entries named `Item_1`, `Item_2`, `Item_3`, running `item_1.cmd`,
`item_2.exe` and `set %item_3%=0` respectively.

Note that nothing there is quoted. That is deliberate and it works, because of
section 2.

---

## 2. `um` reads its raw command line, not `argv`

Most programs let the C runtime chop the command line into `argv[]` on
whitespace. `um` does not. It calls `GetCommandLineW()`, steps over its own
program name, and treats **everything after that as one single string** which
it parses itself.

This matters, and it is the reason the unquoted example above works:

* The C runtime would have handed us
  `["Item_1,", "Item_2,", "Item_3:item_1.cmd,", "item_2.exe,", "set", "%item_3%=0"]`
  — the spaces after your commas would have become argument boundaries, and
  `set %item_3%=0` would have arrived as two separate arguments with the space
  between them lost.
* Reading the raw line instead means the spacing, the commas and the colon are
  all still there exactly as you typed them, and `um` gets to decide what they
  mean.

**Consequence to be aware of:** `cmd.exe` still does its own expansion *before*
`um` is launched. `%item_3%` is substituted by the shell, not by `um`. If
`item_3` is not currently defined, `cmd` replaces `%item_3%` with nothing and
`um` receives `set =0`. In a batch file use `^%item_3^%` or `%%item_3%%`, or
quote the field, if you want the literal text to survive. See
`docs/GOTCHAS.md`.

---

## 3. Splitting names from commands

`um` scans the spec left to right for the **first colon** that is

* not inside quotes (section 5), and
* not part of a doubled `::`.

Everything before it is the **names section**. Everything after it is the
**commands section**.

Because only the *first* colon splits, colons in the commands section need no
escaping at all:

```
um Recover, Cancel : C:\Windows\System32\diskpart.exe /s X:\recover.txt, exit
```

`C:\Windows\...` and `X:\recover.txt` pass through untouched. So does
`http://example.com`. **Colons in the commands section are always literal.**

To put a literal colon in a *name*, double it:

```
um Step 1:: Wipe, Step 2:: Restore : wipe.cmd, restore.cmd
```

gives menu entries `Step 1: Wipe` and `Step 2: Restore`.

If there is no colon anywhere, that is an error — `um` will not guess.

---

## 4. Splitting each section into fields

Both sections are split on **commas** that are not inside quotes and not part
of a doubled `,,`.

Each field is trimmed of leading and trailing whitespace, so
`A, B, C` and `A,B,C` are identical. Whitespace *inside* quotes is preserved
exactly.

The Nth name is paired with the Nth command. **The two counts must match**, and
`um` reports the mismatch with both counts rather than silently truncating.

An **empty command** is legal and means "just select this item and do nothing":

```
um Install, Repair, Quit : setup.exe, repair.cmd,
```

`Quit` has an empty command — nothing runs, but the exit code and `-e`/`-n`
output still tell the caller which item was chosen.

---

## 5. Quoting

A double quote toggles "inside quotes" state. Inside quotes, commas and colons
are **never** separators.

Whether the quotes are *removed* depends on where they are:

### 5a. Whole-field quoting — quotes are stripped

If the first non-whitespace character of a field is `"`, the field is a quoted
field and its outer quotes are removed:

```
um "Wipe, then restore", Cancel : "diskpart /s a.txt, imagex /apply b.wim", exit
```

The first name is `Wipe, then restore` and the first command is
`diskpart /s a.txt, imagex /apply b.wim`, commas and all.

Inside a quoted field, `""` produces one literal quote:

```
"dir ""C:\Program Files"""      →      dir "C:\Program Files"
```

### 5b. Mid-field quotes — quotes are kept

If a quote appears anywhere other than the start of the field, it is **kept
verbatim**, but it still protects commas and colons from being treated as
separators.

This is the case that matters most on Windows, where commands are full of
quoted paths:

```
um Open : notepad "C:\Users\Admin\My Notes, draft.txt"
```

The command handed to `cmd` is
`notepad "C:\Users\Admin\My Notes, draft.txt"` — quotes intact, and the comma
inside them did not split the field.

**Rule of thumb:** quotes you type in the middle of a command survive to the
command. Quotes you wrap a whole field in are `um`'s and get eaten.

---

## 6. Doubling as a lightweight escape

Outside quotes:

| You type | You get | Where it applies |
|---|---|---|
| `,,` | a literal comma | both sections |
| `::` | a literal colon | names section only (colons are already literal after the split) |

```
um Sort A,,Z, Cancel : "dir /o:n", exit
```

The first menu entry is named `Sort A,Z`.

Doubling is scanned strictly left to right: at a `,`, if the next character is
also `,`, both are consumed and one literal comma is emitted. So `A,,B` is the
single field `A,B`, **not** two fields with an empty one between them. If you
want an explicitly empty field, write `""`:

```
um A, "", B : x.cmd, y.cmd, z.cmd
```

Anything more tangled than one doubled comma is much easier to read quoted.
Quoting is the recommended form; doubling is a convenience for simple cases.

---

## 7. Options

Options come **before** the spec. Scanning stops at the first token that does
not begin with `-` or `/`; everything from there to the end of the line is the
spec, parsed as one string per section 2.

`--` ends option scanning explicitly, which is how you start a menu name with
a dash:

```
um -t "Pick one" -- -verbose, -quiet : set V=1, set V=0
```

| Option | Long form | Argument | Meaning |
|---|---|---|---|
| `-t` | `--title` | text | Heading drawn above the menu |
| `-d` | `--default` | 1-based index | Item highlighted at startup, and the item chosen on timeout |
| `-w` | `--wait` | seconds | Auto-pick after this long with no keypress |
| `-e` | `--emit` | — | Print the chosen **command** to stdout instead of running it |
| `-n` | `--name` | — | Print the chosen **name** to stdout instead of running it |
| `-f` | `--file` | path | Read the menu from a file instead of the command line |
| `-x` | `--exit-index` | — | In run mode, exit with the selection index instead of the command's exit code |
| `-1` | `--instant` | — | A hotkey selects immediately instead of just moving the highlight |
| `-h` | `--help`, `/?` | — | Usage text |
| `-V` | `--version` | — | Version |
| | `--dump` | — | Parse the spec, print what was understood, run nothing. Use this when quoting misbehaves. |

Options may be introduced with either `-` or `/`, so `/t` and `-t` are the
same. Long forms take `--`. An option that takes an argument accepts either
`-t Title` or `-t"Title"`; if the argument contains spaces, quote it.

---

## 8. Menu files (`-f`)

One item per line:

```
# Lines starting with # or ; are comments.
!title Recovery Menu

Restore system image : X:\scripts\restore.cmd
Partition disk 0     : diskpart /s X:\scripts\wipe.txt
--------
Command prompt       : cmd.exe
Reboot               : wpeutil reboot
```

* Blank lines are ignored.
* `#` or `;` as the first non-blank character is a comment.
* `!title <text>` sets the menu title (same as `-t`; whichever is given last
  on the command line wins over the file).
* A line of two or more dashes and nothing else is a **separator** — drawn as a
  divider, skipped when navigating, and it does not consume an index or hotkey.
* Everything else is `Name : command`, split on the first colon, both halves
  trimmed. Quoting rules from section 5 apply to each half; comma rules do
  **not** — in a menu file a line is exactly one item, so commas are always
  literal.

Files may be UTF-8 (with or without BOM), UTF-16LE/BE with BOM, or the system
ANSI code page. CRLF and LF line endings both work.

---

## 9. Exit codes

| Code | Meaning |
|---|---|
| child's code | Run mode: whatever the command returned |
| 1..N | The 1-based index of the chosen item (`-e`, `-n`, `-x`, or an empty command) |
| 0 | Cancelled with Esc, or timed out with no default set |
| 255 | Usage or parse error — nothing ran |

Cancelling always exits 0 and always runs nothing, in every mode.

---

## 10. Things `um` deliberately does not do

* It does not expand environment variables. That is the shell's job, and doing
  it twice causes more surprises than it solves.
* It does not interpret the command at all — the string is handed to
  `cmd /c` (run mode) or printed verbatim (emit mode).
* It does not guess when the name and command counts disagree.
