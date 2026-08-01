/*
 * um_win.h -- the Windows-specific half of `um`: console rendering, key
 * handling, command execution and file loading.
 *
 * Everything that touches Win32 lives here, so that um_parse.c can stay
 * portable and testable on the build host.
 */
#ifndef UM_WIN_H
#define UM_WIN_H

#include "um_parse.h"

typedef struct {
    const wchar_t *title;   /* heading, or NULL                              */
    int default_sel;        /* 0-based SELECTABLE ordinal, -1 for none       */
    int wait_secs;          /* 0 = wait forever                              */
    int instant;            /* non-zero: a hotkey selects without Enter      */
} um_ui_opts;

/*
 * Show the menu and return the 0-based index into m->items of the chosen
 * entry, or -1 if the user cancelled (Esc / Ctrl+C) or the timeout expired
 * with no default set.
 *
 * The menu is drawn to CONOUT$ and keys are read from CONIN$, deliberately
 * bypassing stdout/stdin. That is what lets `um -e` be used inside a
 * for /f pipeline: the interface still appears on screen while stdout carries
 * nothing but the chosen command. If a real console cannot be opened, this
 * falls back to a plain numbered prompt on stderr + stdin.
 */
int um_run_menu(const um_menu *m, const um_ui_opts *o);

/* Run `cmd` via the command interpreter. Returns the child's exit code. */
int um_exec(const wchar_t *cmd);

/* Write to stdout (console-aware) followed by CRLF. Used by -e and -n. */
void um_emit(const wchar_t *text);

/* Write a line to stderr. Used for errors and usage. */
void um_err(const wchar_t *text);

/* Write a line to the console proper, falling back to stderr. */
void um_note(const wchar_t *text);

/*
 * Read a menu file and decode it to wide characters. Handles UTF-8 (with or
 * without BOM), UTF-16LE/BE with BOM, and the system ANSI code page.
 * Returns NULL on failure with *err set to an owned message.
 */
wchar_t *um_read_file_text(const wchar_t *path, wchar_t **err);

#endif /* UM_WIN_H */
