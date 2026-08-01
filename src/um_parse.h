/*
 * um_parse.h -- menu-spec parsing for `um`.
 *
 * This header (and um_parse.c) contain NO Win32 calls. That is on purpose:
 * the parser is the fiddly part, so it is kept portable and compiled natively
 * on the build host by test/test_parse.c, where it can be exercised quickly
 * without a Windows machine in the loop.
 *
 * Everything is wchar_t based. Note that wchar_t is 2 bytes on Windows and 4
 * on most Unixes; the parser never inspects surrogate pairs or does arithmetic
 * on code units, so it behaves identically on both.
 *
 * The syntax implemented here is specified in docs/SYNTAX.md.
 */
#ifndef UM_PARSE_H
#define UM_PARSE_H

#include <stddef.h>
#include <wchar.h>

#define UM_VERSION L"0.4.0"

/* A single menu entry. */
typedef struct {
    wchar_t *name;          /* display label; owned                          */
    wchar_t *cmd;           /* command text; owned; may be L"" (do nothing)  */
    int      is_separator;  /* non-zero: a divider, not selectable           */
} um_item;

/* A parsed menu. Free with um_menu_free(). */
typedef struct {
    um_item *items;
    int      count;         /* including separators                          */
    wchar_t *title;         /* owned, may be NULL                            */
    wchar_t *error;         /* owned; NULL means the parse succeeded         */
} um_menu;

/*
 * Parse a command-line spec of the form
 *
 *     name, name, name : command, command, command
 *
 * `spec` is the raw text after any options -- see docs/SYNTAX.md section 2.
 * Never returns NULL; on failure the returned menu has ->error set and
 * ->count == 0.
 */
um_menu *um_parse_spec(const wchar_t *spec);

/*
 * Parse the contents of a menu file: one `name : command` per line, plus
 * comments, blank lines, `----` separators and the `!title` directive.
 * `text` is the whole file already decoded to wide characters.
 */
um_menu *um_parse_file_text(const wchar_t *text);

/*
 * Trim the spec and, if a single pair of quotes wraps the whole of it, remove
 * that pair. Returns an owned string; caller frees.
 *
 * This exists because both forms are natural to type and both must work:
 *
 *     um A, B : a.cmd, b.cmd          <- bare
 *     um "A, B : a.cmd, b.cmd"        <- wrapped, which is what people write
 *                                        inside a for /f, and what the help
 *                                        text itself shows
 *
 * The pair is only removed when the quote opening the string is the one
 * closing it at the very end, so a spec whose FIRST FIELD happens to be quoted
 * --  um "Wipe, then restore", Cancel : go.cmd, exit  -- is left alone.
 */
wchar_t *um_unwrap_spec(const wchar_t *spec);

void um_menu_free(um_menu *m);

/*
 * Index of the Nth selectable item (0-based n), or -1 if there are fewer than
 * n+1 selectable items. Used to map hotkeys and --default, which count only
 * real entries, onto the items array, which also holds separators.
 */
int um_selectable_index(const um_menu *m, int n);

/* How many entries are actually selectable (i.e. not separators). */
int um_selectable_count(const um_menu *m);

/*
 * The hotkey character for the nth selectable item: '1'..'9' for the first
 * nine, then 'a'..'z'. Returns 0 once we run out of keys, which is not an
 * error -- such items are still reachable with the arrow keys.
 */
wchar_t um_hotkey_for(int n);

/* Inverse of um_hotkey_for: character -> selectable ordinal, or -1. */
int um_hotkey_ordinal(wchar_t ch);

#endif /* UM_PARSE_H */
