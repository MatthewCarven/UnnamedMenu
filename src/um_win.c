/*
 * um_win.c -- console rendering, key handling, execution and file loading.
 *
 * Design notes worth knowing before editing this file:
 *
 * 1. The menu is drawn to CONOUT$ and read from CONIN$, opened explicitly with
 *    CreateFileW rather than taken from GetStdHandle. This is the whole trick
 *    behind `um -e` working inside
 *
 *        for /f "delims=" %%c in ('um -e "A,B:set X=1,set X=2"') do %%c
 *
 *    Under that construct cmd replaces our stdout with a pipe. If we drew the
 *    menu to stdout the user would see nothing and the captured text would be
 *    the entire interface. Talking to the console device directly keeps the
 *    two channels completely separate.
 *
 * 2. Colour is done with SetConsoleTextAttribute, not ANSI/VT escapes. WinPE's
 *    conhost cannot be relied upon to have VT processing enabled, and the
 *    attribute API has worked since NT 3.1.
 *
 * 3. Redrawing is done in place: remember where the menu started, and on each
 *    keypress move the cursor back there and repaint every line, padded to the
 *    console width so stale text is erased. After the first paint the origin is
 *    recomputed from the current cursor position, which self-corrects if the
 *    buffer scrolled while the menu was being drawn.
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "um_win.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Console state                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    HANDLE out;
    HANDLE in;
    int    interactive;

    DWORD  saved_in_mode;
    int    saved_in_mode_ok;
    CONSOLE_CURSOR_INFO saved_cursor;
    int    saved_cursor_ok;

    WORD   attr_normal;
    WORD   attr_highlight;
    WORD   attr_dim;
    WORD   attr_title;

    SHORT  width;
    SHORT  origin_y;
    int    lines_drawn;
} um_console;

/* Kept in a static as well so the Ctrl-handler can put things back. */
static um_console *g_con = NULL;

static void con_restore(um_console *c)
{
    if (!c) return;
    if (c->saved_cursor_ok && c->out != INVALID_HANDLE_VALUE)
        SetConsoleCursorInfo(c->out, &c->saved_cursor);
    if (c->saved_in_mode_ok && c->in != INVALID_HANDLE_VALUE)
        SetConsoleMode(c->in, c->saved_in_mode);
    if (c->out != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute(c->out, c->attr_normal);
}

static BOOL WINAPI ctrl_handler(DWORD type)
{
    (void)type;
    con_restore(g_con);
    return FALSE;             /* let the default handler end the process */
}

static void con_open(um_console *c)
{
    CONSOLE_SCREEN_BUFFER_INFO info;

    memset(c, 0, sizeof *c);
    c->out = INVALID_HANDLE_VALUE;
    c->in = INVALID_HANDLE_VALUE;
    c->attr_normal = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    c->width = 80;

    c->out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                         OPEN_EXISTING, 0, NULL);
    c->in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);

    if (c->out == INVALID_HANDLE_VALUE || c->in == INVALID_HANDLE_VALUE)
        return;                                  /* stays non-interactive */

    if (!GetConsoleScreenBufferInfo(c->out, &info))
        return;

    c->attr_normal = info.wAttributes;
    /* Reverse video by swapping the fg/bg nibbles -- more portable than
       COMMON_LVB_REVERSE_VIDEO, which older conhosts ignore. */
    c->attr_highlight = (WORD)(((info.wAttributes & 0x0F) << 4) |
                               ((info.wAttributes & 0xF0) >> 4));
    c->attr_dim = (WORD)((info.wAttributes & 0xF0) | FOREGROUND_INTENSITY);
    c->attr_title = (WORD)(info.wAttributes | FOREGROUND_INTENSITY);
    c->width = info.dwSize.X;
    if (c->width < 20) c->width = 20;
    if (c->width > 500) c->width = 500;
    c->origin_y = info.dwCursorPosition.Y;

    if (GetConsoleMode(c->in, &c->saved_in_mode)) {
        c->saved_in_mode_ok = 1;
        /* No line editing, no echo. ENABLE_PROCESSED_INPUT is switched off so
           Ctrl+C arrives as a key event we can treat as "cancel" instead of
           killing us mid-draw. */
        SetConsoleMode(c->in, ENABLE_EXTENDED_FLAGS);
    }
    if (GetConsoleCursorInfo(c->out, &c->saved_cursor)) {
        CONSOLE_CURSOR_INFO hidden = c->saved_cursor;
        c->saved_cursor_ok = 1;
        hidden.bVisible = FALSE;
        SetConsoleCursorInfo(c->out, &hidden);
    }

    c->interactive = 1;
    g_con = c;
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
}

static void con_close(um_console *c)
{
    con_restore(c);
    SetConsoleCtrlHandler(ctrl_handler, FALSE);
    g_con = NULL;
    if (c->out != INVALID_HANDLE_VALUE) CloseHandle(c->out);
    if (c->in != INVALID_HANDLE_VALUE) CloseHandle(c->in);
    c->out = c->in = INVALID_HANDLE_VALUE;
}

/* ------------------------------------------------------------------ */
/* Raw output helpers                                                  */
/* ------------------------------------------------------------------ */

static void write_console_w(HANDLE h, const wchar_t *s, size_t n)
{
    DWORD written;
    if (h == INVALID_HANDLE_VALUE || !s || !n) return;
    WriteConsoleW(h, s, (DWORD)n, &written, NULL);
}

/*
 * Write wide text to a std handle, choosing the right path for whatever is on
 * the other end: WriteConsoleW when it really is a console (so Unicode
 * survives), otherwise encode to the console output code page, which is what
 * cmd.exe uses when it reads a pipe -- getting this wrong is how accented
 * characters turn to mojibake inside `for /f`.
 */
static void write_handle_w(HANDLE h, const wchar_t *s)
{
    DWORD mode, written;
    int need;
    char *buf;
    UINT cp;

    if (h == NULL || h == INVALID_HANDLE_VALUE || !s) return;

    if (GetConsoleMode(h, &mode)) {
        WriteConsoleW(h, s, (DWORD)wcslen(s), &written, NULL);
        return;
    }

    cp = GetConsoleOutputCP();
    if (cp == 0) cp = GetACP();

    need = WideCharToMultiByte(cp, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 1) return;
    buf = (char *)malloc((size_t)need);
    if (!buf) return;
    WideCharToMultiByte(cp, 0, s, -1, buf, need, NULL, NULL);
    WriteFile(h, buf, (DWORD)(need - 1), &written, NULL);   /* drop the NUL */
    free(buf);
}

void um_emit(const wchar_t *text)
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    write_handle_w(h, text ? text : L"");
    write_handle_w(h, L"\r\n");
}

void um_err(const wchar_t *text)
{
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    write_handle_w(h, text ? text : L"");
    write_handle_w(h, L"\r\n");
}

void um_note(const wchar_t *text)
{
    HANDLE h = CreateFileW(L"CONOUT$", GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { um_err(text); return; }
    write_handle_w(h, text ? text : L"");
    write_handle_w(h, L"\r\n");
    CloseHandle(h);
}

/* ------------------------------------------------------------------ */
/* Menu rendering                                                      */
/* ------------------------------------------------------------------ */

/* Pad or truncate `text` to width-1 columns and write it as one attributed
   line. Fixed width means a redraw overwrites whatever was there before. */
static void draw_line(um_console *c, WORD attr, const wchar_t *text)
{
    int w = c->width - 1;
    wchar_t *line = (wchar_t *)malloc((size_t)(w + 3) * sizeof(wchar_t));
    int i = 0;

    if (!line) return;
    while (text && text[i] && i < w) { line[i] = text[i]; i++; }
    while (i < w) line[i++] = L' ';
    line[i++] = L'\r';
    line[i++] = L'\n';

    SetConsoleTextAttribute(c->out, attr);
    write_console_w(c->out, line, (size_t)i);
    SetConsoleTextAttribute(c->out, c->attr_normal);
    free(line);
}

static void append(wchar_t *dst, size_t cap, size_t *len, const wchar_t *src)
{
    while (*src && *len + 1 < cap) dst[(*len)++] = *src++;
    dst[*len] = L'\0';
}

static void append_ch(wchar_t *dst, size_t cap, size_t *len, wchar_t ch)
{
    if (*len + 1 < cap) { dst[(*len)++] = ch; dst[*len] = L'\0'; }
}

static void append_int(wchar_t *dst, size_t cap, size_t *len, int v)
{
    wchar_t tmp[16];
    int n = 0;
    if (v == 0) { append_ch(dst, cap, len, L'0'); return; }
    while (v > 0 && n < 15) { tmp[n++] = (wchar_t)(L'0' + v % 10); v /= 10; }
    while (n > 0) append_ch(dst, cap, len, tmp[--n]);
}

/*
 * Repaint the whole menu at the recorded origin.
 * `remaining` < 0 means no countdown line.
 */
static void draw_menu(um_console *c, const um_menu *m, const um_ui_opts *o,
                      int cur, int remaining)
{
    COORD at;
    CONSOLE_SCREEN_BUFFER_INFO info;
    wchar_t buf[600];
    size_t len;
    int i, ordinal = 0, lines = 0;

    at.X = 0;
    at.Y = c->origin_y;
    SetConsoleCursorPosition(c->out, at);

    if (o->title && o->title[0]) {
        draw_line(c, c->attr_title, o->title);
        draw_line(c, c->attr_normal, L"");
        lines += 2;
    }

    for (i = 0; i < m->count; i++) {
        int selected = (i == cur);
        len = 0;
        buf[0] = L'\0';

        if (m->items[i].is_separator) {
            int w = c->width - 1;
            int k;
            append(buf, 600, &len, L"  ");
            for (k = 2; k < w - 2 && k < 599; k++) append_ch(buf, 600, &len, L'-');
            draw_line(c, c->attr_dim, buf);
            lines++;
            continue;
        }

        {
            wchar_t hot = um_hotkey_for(ordinal);
            append(buf, 600, &len, selected ? L" > " : L"   ");
            if (hot) {
                append_ch(buf, 600, &len, hot);
                append(buf, 600, &len, L") ");
            } else {
                append(buf, 600, &len, L"   ");
            }
            append(buf, 600, &len, m->items[i].name);
        }
        draw_line(c, selected ? c->attr_highlight : c->attr_normal, buf);
        lines++;
        ordinal++;
    }

    draw_line(c, c->attr_normal, L"");
    lines++;

    len = 0; buf[0] = L'\0';
    append(buf, 600, &len,
           o->instant ? L"   Up/Down move   Enter or hotkey select   Esc cancel"
                      : L"   Up/Down move   Enter select   Esc cancel");
    draw_line(c, c->attr_dim, buf);
    lines++;

    if (remaining >= 0) {
        len = 0; buf[0] = L'\0';
        append(buf, 600, &len, L"   auto-selecting in ");
        append_int(buf, 600, &len, remaining);
        append(buf, 600, &len, L"s -- press any key to stop");
        draw_line(c, c->attr_dim, buf);
        lines++;
    }

    c->lines_drawn = lines;

    /* If the buffer scrolled while drawing, our stored origin is now wrong.
       Back-compute it from where the cursor actually ended up. */
    if (GetConsoleScreenBufferInfo(c->out, &info)) {
        SHORT want = (SHORT)(info.dwCursorPosition.Y - lines);
        if (want < 0) want = 0;
        c->origin_y = want;
    }
}

/* Move the cursor below the menu so subsequent output does not overwrite it. */
static void park_cursor(um_console *c)
{
    COORD at;
    at.X = 0;
    at.Y = (SHORT)(c->origin_y + c->lines_drawn);
    SetConsoleCursorPosition(c->out, at);
}

/* ------------------------------------------------------------------ */
/* Navigation helpers                                                  */
/* ------------------------------------------------------------------ */

static int next_selectable(const um_menu *m, int from, int dir)
{
    int i = from;
    int guard = m->count + 1;
    while (guard-- > 0) {
        i += dir;
        if (i < 0) i = m->count - 1;
        if (i >= m->count) i = 0;
        if (!m->items[i].is_separator) return i;
    }
    return from;
}

static int first_selectable(const um_menu *m)
{
    int i;
    for (i = 0; i < m->count; i++)
        if (!m->items[i].is_separator) return i;
    return -1;
}

static int last_selectable(const um_menu *m)
{
    int i;
    for (i = m->count - 1; i >= 0; i--)
        if (!m->items[i].is_separator) return i;
    return -1;
}

/* ------------------------------------------------------------------ */
/* Interactive loop                                                    */
/* ------------------------------------------------------------------ */

/*
 * Input pacing.
 *
 * The obvious loop -- WaitForSingleObject on the console input handle, then
 * ReadConsoleInputW -- looks right and is not. A console input handle signals
 * for ANY input record, including mouse movement, focus changes and buffer
 * resizes, and it can also report itself signalled while the queue is
 * effectively empty. When that happens the following ReadConsoleInputW blocks
 * indefinitely, the countdown stops advancing and the menu hangs until a key
 * is pressed -- which entirely defeats an unattended -w timeout.
 *
 * So: never call ReadConsoleInputW without first confirming with
 * GetNumberOfConsoleInputEvents that a record is actually queued. The wait is
 * kept only as a way to sleep politely, and its result is not trusted. If it
 * claims the handle is signalled but the queue is empty, sleep briefly rather
 * than spinning the CPU.
 *
 * Time remaining is computed from GetTickCount() rather than accumulated from
 * the poll interval, so the countdown stays accurate no matter how the waits
 * actually land.
 */
#define POLL_TIMED_MS   50    /* while a countdown is running   */
#define POLL_IDLE_MS   200    /* while waiting indefinitely     */
#define POLL_EMPTY_MS   10    /* signalled-but-empty backstop   */

/*
 * Is this keypress worth reacting to?
 *
 * Two reasons to filter. First, a key-down for a bare modifier (Shift, Ctrl,
 * Alt, Caps Lock...) is not a decision, and on an unattended recovery drive a
 * nudged keyboard must not be able to cancel the auto-selection countdown and
 * leave the machine sitting at a menu forever. Second, some console
 * implementations queue synthetic key records around focus and stream changes;
 * those carry no character and no navigation meaning, and should be ignored
 * for the same reason.
 */
static int key_is_meaningful(const KEY_EVENT_RECORD *k)
{
    switch (k->wVirtualKeyCode) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU:
    case VK_LWIN:  case VK_RWIN:    case VK_APPS:
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL:
        return 0;
    case VK_UP: case VK_DOWN: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_RETURN: case VK_ESCAPE:
        return 1;
    default: {
        wchar_t ch = k->uChar.UnicodeChar;
        return ch >= 32 || ch == 3 /* ^C */ || ch == 13 || ch == 27;
    }
    }
}

static int menu_interactive(um_console *c, const um_menu *m,
                            const um_ui_opts *o)
{
    int cur = first_selectable(m);
    int timeout_ms = o->wait_secs > 0 ? o->wait_secs * 1000 : 0;
    DWORD started = GetTickCount();
    int last_secs = -2;          /* -1 is a real value (no countdown shown) */
    int need_draw = 1;
    int result = -1;

    if (cur < 0) return -1;

    if (o->default_sel >= 0) {
        int idx = um_selectable_index(m, o->default_sel);
        if (idx >= 0) cur = idx;
    }

    for (;;) {
        INPUT_RECORD rec;
        DWORD got = 0, avail = 0, wr;
        int secs = -1;

        if (timeout_ms > 0) {
            int left = timeout_ms - (int)(GetTickCount() - started);
            if (left <= 0) {
                /* Timed out. Take the default if one was given; otherwise
                   treat it as a cancel rather than choosing for the user. */
                result = (o->default_sel >= 0) ? cur : -1;
                break;
            }
            secs = (left + 999) / 1000;
        }

        if (need_draw || secs != last_secs) {
            draw_menu(c, m, o, cur, secs);
            last_secs = secs;
            need_draw = 0;
        }

        wr = WaitForSingleObject(c->in, timeout_ms > 0 ? POLL_TIMED_MS
                                                      : POLL_IDLE_MS);
        if (!GetNumberOfConsoleInputEvents(c->in, &avail)) { result = -1; break; }
        if (avail == 0) {
            if (wr == WAIT_OBJECT_0) Sleep(POLL_EMPTY_MS);
            continue;
        }

        if (!ReadConsoleInputW(c->in, &rec, 1, &got) || got == 0) continue;

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (GetConsoleScreenBufferInfo(c->out, &info)) {
                c->width = info.dwSize.X;
                if (c->width < 20) c->width = 20;
                if (c->width > 500) c->width = 500;
            }
            need_draw = 1;
            continue;
        }
        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
            continue;
        if (!key_is_meaningful(&rec.Event.KeyEvent))
            continue;

        /* A deliberate keypress means someone is here: stop the countdown. */
        if (timeout_ms > 0) { timeout_ms = 0; need_draw = 1; }

        switch (rec.Event.KeyEvent.wVirtualKeyCode) {
        case VK_UP:
            cur = next_selectable(m, cur, -1); need_draw = 1; break;
        case VK_DOWN:
            cur = next_selectable(m, cur, +1); need_draw = 1; break;
        case VK_HOME:
        case VK_PRIOR:
            cur = first_selectable(m); need_draw = 1; break;
        case VK_END:
        case VK_NEXT:
            cur = last_selectable(m); need_draw = 1; break;
        case VK_RETURN:
            result = cur; goto done;
        case VK_ESCAPE:
            result = -1; goto done;
        default: {
            wchar_t ch = rec.Event.KeyEvent.uChar.UnicodeChar;
            int ord;

            if (ch == 3 || ch == 27) { result = -1; goto done; }  /* ^C, Esc */

            ord = um_hotkey_ordinal(ch);
            if (ord >= 0) {
                int idx = um_selectable_index(m, ord);
                if (idx >= 0) {
                    cur = idx;
                    need_draw = 1;
                    if (o->instant) { result = cur; goto done; }
                }
            }
            break;
        }
        }
    }

done:
    /* Final repaint with the countdown line gone, then step past the menu so
       whatever runs next does not overwrite it. */
    draw_menu(c, m, o, result >= 0 ? result : cur, -1);
    park_cursor(c);
    return result;
}

/* ------------------------------------------------------------------ */
/* Non-interactive fallback                                            */
/* ------------------------------------------------------------------ */

/* Read one line from stdin, decoded from the console input code page. */
static int read_stdin_line(wchar_t *out, size_t cap)
{
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    char raw[512];
    DWORD got = 0;
    size_t n = 0;
    UINT cp;

    if (h == NULL || h == INVALID_HANDLE_VALUE) return 0;

    while (n < sizeof raw - 1) {
        char ch;
        if (!ReadFile(h, &ch, 1, &got, NULL) || got == 0) break;
        if (ch == '\n') break;
        if (ch == '\r') continue;
        raw[n++] = ch;
    }
    raw[n] = '\0';
    if (n == 0 && got == 0) return 0;              /* EOF with nothing read */

    cp = GetConsoleCP();
    if (cp == 0) cp = GetACP();
    MultiByteToWideChar(cp, 0, raw, -1, out, (int)cap);
    return 1;
}

static int menu_fallback(const um_menu *m, const um_ui_opts *o)
{
    wchar_t buf[512];
    int attempt;
    int ordinal;
    int i;

    for (attempt = 0; attempt < 3; attempt++) {
        ordinal = 0;
        if (o->title && o->title[0]) { um_note(o->title); um_note(L""); }

        for (i = 0; i < m->count; i++) {
            wchar_t line[600];
            size_t len = 0;
            wchar_t hot;

            line[0] = L'\0';
            if (m->items[i].is_separator) { um_note(L"   ---"); continue; }

            hot = um_hotkey_for(ordinal);
            append(line, 600, &len, L"   ");
            if (hot) { append_ch(line, 600, &len, hot); append(line, 600, &len, L") "); }
            append(line, 600, &len, m->items[i].name);
            if (o->default_sel == ordinal) append(line, 600, &len, L"   [default]");
            um_note(line);
            ordinal++;
        }
        um_note(L"");
        um_note(L"   Choose an item and press Enter (blank cancels):");

        if (!read_stdin_line(buf, 512)) break;      /* EOF */

        {
            wchar_t *p = buf;
            int ord;
            while (*p == L' ' || *p == L'\t') p++;
            if (!*p) break;                          /* blank line */
            ord = um_hotkey_ordinal(*p);
            /* Multi-digit entries: 12 should mean item 12, not item 1. */
            if (*p >= L'1' && *p <= L'9') {
                long v = wcstol(p, NULL, 10);
                if (v >= 1) ord = (int)v - 1;
            }
            if (ord >= 0) {
                int idx = um_selectable_index(m, ord);
                if (idx >= 0) return idx;
            }
        }
        um_note(L"   Not one of the choices -- try again.");
        um_note(L"");
    }

    /* No usable answer: fall back to the default if one was given. */
    if (o->default_sel >= 0) return um_selectable_index(m, o->default_sel);
    return -1;
}

int um_run_menu(const um_menu *m, const um_ui_opts *o)
{
    um_console c;
    int r;

    if (!m || m->count == 0) return -1;

    con_open(&c);
    if (!c.interactive) {
        con_close(&c);
        return menu_fallback(m, o);
    }
    r = menu_interactive(&c, m, o);
    con_close(&c);
    return r;
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

/*
 * Run `cmd` through the command interpreter.
 *
 * The command line built here is
 *
 *     "<comspec>" /s /c "<command>"
 *
 * The /s switch combined with wrapping the command in quotes tells cmd to
 * strip exactly the first and last quote and use everything between them
 * verbatim. Without /s, cmd applies a convoluted rule about how many quotes
 * are present and will happily mangle a command containing quoted paths.
 */
int um_exec(const wchar_t *cmd)
{
    wchar_t comspec[MAX_PATH * 2];
    wchar_t *line;
    size_t n;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;

    if (!cmd || !cmd[0]) return 0;

    if (!GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH * 2)) {
        UINT len = GetSystemDirectoryW(comspec, MAX_PATH);
        if (len == 0 || len > MAX_PATH) return 255;
        wcscat(comspec, L"\\cmd.exe");
    }

    n = wcslen(comspec) + wcslen(cmd) + 32;
    line = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!line) return 255;

    wcscpy(line, L"\"");
    wcscat(line, comspec);
    wcscat(line, L"\" /s /c \"");
    wcscat(line, cmd);
    wcscat(line, L"\"");

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    memset(&pi, 0, sizeof pi);

    if (!CreateProcessW(comspec, line, NULL, NULL, TRUE, 0, NULL, NULL,
                        &si, &pi)) {
        free(line);
        um_err(L"um: could not start the command interpreter");
        return 255;
    }
    free(line);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

/* ------------------------------------------------------------------ */
/* Menu files                                                          */
/* ------------------------------------------------------------------ */

#define UM_MAX_FILE (1024u * 1024u)

wchar_t *um_read_file_text(const wchar_t *path, wchar_t **err)
{
    HANDLE h;
    DWORD size, got = 0;
    unsigned char *raw;
    wchar_t *text = NULL;
    int wide_chars;
    UINT cp;
    const char *body;
    int body_len;

    if (err) *err = NULL;

    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = _wcsdup(L"cannot open the menu file");
        return NULL;
    }

    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > UM_MAX_FILE) {
        CloseHandle(h);
        if (err) *err = _wcsdup(L"menu file is unreadable or larger than 1 MB");
        return NULL;
    }

    raw = (unsigned char *)malloc(size + 2);
    if (!raw) { CloseHandle(h); if (err) *err = _wcsdup(L"out of memory"); return NULL; }

    if (!ReadFile(h, raw, size, &got, NULL)) {
        CloseHandle(h); free(raw);
        if (err) *err = _wcsdup(L"could not read the menu file");
        return NULL;
    }
    CloseHandle(h);
    raw[got] = 0;
    raw[got + 1] = 0;

    /* UTF-16 with a BOM: use it directly, byte-swapping for big endian. */
    if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        wide_chars = (int)((got - 2) / 2);
        text = (wchar_t *)malloc(((size_t)wide_chars + 1) * sizeof(wchar_t));
        if (text) {
            int i;
            for (i = 0; i < wide_chars; i++)
                text[i] = (wchar_t)(raw[2 + i * 2] | (raw[3 + i * 2] << 8));
            text[wide_chars] = 0;
        }
        free(raw);
        if (!text && err) *err = _wcsdup(L"out of memory");
        return text;
    }
    if (got >= 2 && raw[0] == 0xFE && raw[1] == 0xFF) {
        wide_chars = (int)((got - 2) / 2);
        text = (wchar_t *)malloc(((size_t)wide_chars + 1) * sizeof(wchar_t));
        if (text) {
            int i;
            for (i = 0; i < wide_chars; i++)
                text[i] = (wchar_t)((raw[2 + i * 2] << 8) | raw[3 + i * 2]);
            text[wide_chars] = 0;
        }
        free(raw);
        if (!text && err) *err = _wcsdup(L"out of memory");
        return text;
    }

    /* Otherwise it is bytes. Skip a UTF-8 BOM if present, then try strict
       UTF-8; if the file is not valid UTF-8 it is almost certainly in the
       system ANSI code page, so decode it that way instead of failing. */
    body = (const char *)raw;
    body_len = (int)got;
    if (got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        body += 3;
        body_len -= 3;
    }

    cp = CP_UTF8;
    wide_chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     body, body_len, NULL, 0);
    if (wide_chars <= 0) {
        cp = GetACP();
        wide_chars = MultiByteToWideChar(cp, 0, body, body_len, NULL, 0);
    }
    if (wide_chars <= 0) {
        free(raw);
        if (err) *err = _wcsdup(L"menu file is empty");
        return NULL;
    }

    text = (wchar_t *)malloc(((size_t)wide_chars + 1) * sizeof(wchar_t));
    if (!text) { free(raw); if (err) *err = _wcsdup(L"out of memory"); return NULL; }
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0,
                        body, body_len, text, wide_chars);
    text[wide_chars] = 0;
    free(raw);
    return text;
}
