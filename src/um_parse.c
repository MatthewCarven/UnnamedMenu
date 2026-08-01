/*
 * um_parse.c -- menu-spec parsing for `um`.  See docs/SYNTAX.md for the spec
 * this file implements, and um_parse.h for why it contains no Win32 calls.
 */
#include "um_parse.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define UM_MAX_ITEMS 999

/* ------------------------------------------------------------------ */
/* Small wide-string helpers                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t *p;
    size_t   len;
    size_t   cap;
} WBuf;

static int wbuf_init(WBuf *b)
{
    b->cap = 64;
    b->len = 0;
    b->p = (wchar_t *)malloc(b->cap * sizeof(wchar_t));
    if (!b->p) return 0;
    b->p[0] = L'\0';
    return 1;
}

static int wbuf_push(WBuf *b, wchar_t c)
{
    if (b->len + 2 > b->cap) {
        size_t ncap = b->cap * 2;
        wchar_t *np = (wchar_t *)realloc(b->p, ncap * sizeof(wchar_t));
        if (!np) return 0;
        b->p = np;
        b->cap = ncap;
    }
    b->p[b->len++] = c;
    b->p[b->len] = L'\0';
    return 1;
}

static wchar_t *wdup_n(const wchar_t *s, size_t n)
{
    wchar_t *r = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!r) return NULL;
    if (n) memcpy(r, s, n * sizeof(wchar_t));
    r[n] = L'\0';
    return r;
}

static wchar_t *wdup(const wchar_t *s)
{
    return s ? wdup_n(s, wcslen(s)) : NULL;
}

static int is_space(wchar_t c)
{
    return c == L' ' || c == L'\t';
}

/* Allocating sprintf, so error messages can carry useful detail. */
static wchar_t *wfmt(const wchar_t *fmt, ...)
{
    wchar_t tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(tmp, (sizeof tmp / sizeof tmp[0]), fmt, ap);
    va_end(ap);
    tmp[(sizeof tmp / sizeof tmp[0]) - 1] = L'\0';
    return wdup(tmp);
}

static um_menu *menu_new(void)
{
    um_menu *m = (um_menu *)calloc(1, sizeof(um_menu));
    return m;
}

static um_menu *menu_error(um_menu *m, wchar_t *msg)
{
    if (!m) return NULL;
    free(m->error);
    m->error = msg;
    return m;
}

void um_menu_free(um_menu *m)
{
    int i;
    if (!m) return;
    for (i = 0; i < m->count; i++) {
        free(m->items[i].name);
        free(m->items[i].cmd);
    }
    free(m->items);
    free(m->title);
    free(m->error);
    free(m);
}

/* ------------------------------------------------------------------ */
/* Field scanning                                                      */
/* ------------------------------------------------------------------ */

/*
 * Read one field starting at s[*i] and running to either a separating comma
 * or the end of the buffer.
 *
 * Quote handling is the interesting part and follows docs/SYNTAX.md section 5:
 *
 *   - a quote ALWAYS toggles "inside quotes" state, and inside that state
 *     commas and colons are never separators;
 *   - but the quote characters themselves are only *removed* when the field
 *     began with one ("whole-field quoting").  A quote appearing mid-field is
 *     kept, because on Windows those quotes usually belong to the command
 *     being run -- notepad "C:\Program Files\x.txt" must survive intact.
 *
 * `keep` tracks how much of the buffer to retain when trimming trailing
 * whitespace: whitespace written while inside quotes is significant and must
 * not be trimmed, so `keep` advances for it; whitespace outside quotes does
 * not advance `keep` and is therefore dropped if the field ends there.
 */
static wchar_t *parse_one_field(const wchar_t *s, size_t len, size_t *i,
                                int collapse_colons, int split_on_comma,
                                int *unterminated)
{
    WBuf b;
    size_t keep = 0;
    int in_quote = 0;
    int quoted_field;

    if (!wbuf_init(&b)) return NULL;

    while (*i < len && is_space(s[*i])) (*i)++;      /* leading whitespace */

    quoted_field = (*i < len && s[*i] == L'"');

    while (*i < len) {
        wchar_t c = s[*i];

        if (c == L'"') {
            /* A doubled quote inside quotes is one literal quote. */
            if (in_quote && *i + 1 < len && s[*i + 1] == L'"') {
                if (!wbuf_push(&b, L'"')) goto oom;
                if (!quoted_field && !wbuf_push(&b, L'"')) goto oom;
                keep = b.len;
                *i += 2;
                continue;
            }
            in_quote = !in_quote;
            if (!quoted_field) {                     /* mid-field: keep it */
                if (!wbuf_push(&b, L'"')) goto oom;
                keep = b.len;
            }
            (*i)++;
            continue;
        }

        if (!in_quote && split_on_comma && c == L',') {
            if (*i + 1 < len && s[*i + 1] == L',') { /* ,, -> literal comma */
                if (!wbuf_push(&b, L',')) goto oom;
                keep = b.len;
                *i += 2;
                continue;
            }
            break;                                    /* field ends here */
        }

        if (!in_quote && collapse_colons && c == L':' &&
            *i + 1 < len && s[*i + 1] == L':') {      /* :: -> literal colon */
            if (!wbuf_push(&b, L':')) goto oom;
            keep = b.len;
            *i += 2;
            continue;
        }

        if (!wbuf_push(&b, c)) goto oom;
        if (!is_space(c) || in_quote) keep = b.len;
        (*i)++;
    }

    if (in_quote && unterminated) *unterminated = 1;

    b.p[keep] = L'\0';                                /* trim trailing space */
    return b.p;

oom:
    free(b.p);
    return NULL;
}

/* Split a whole section into comma-separated fields. */
static int parse_fields(const wchar_t *s, size_t len, int collapse_colons,
                        wchar_t ***out, int *out_n, int *unterminated)
{
    wchar_t **arr = NULL;
    int n = 0, cap = 0;
    size_t i = 0;

    for (;;) {
        wchar_t *f = parse_one_field(s, len, &i, collapse_colons, 1,
                                     unterminated);
        if (!f) goto fail;

        if (n + 1 > cap) {
            int ncap = cap ? cap * 2 : 8;
            wchar_t **na = (wchar_t **)realloc(arr, (size_t)ncap * sizeof(wchar_t *));
            if (!na) { free(f); goto fail; }
            arr = na;
            cap = ncap;
        }
        arr[n++] = f;

        if (n > UM_MAX_ITEMS) goto fail;

        if (i < len && s[i] == L',') { i++; continue; } /* eat separator */
        break;
    }

    *out = arr;
    *out_n = n;
    return 1;

fail:
    while (n > 0) free(arr[--n]);
    free(arr);
    return 0;
}

/*
 * Locate the colon that separates names from commands: the first one that is
 * outside quotes and not part of a doubled `::`.  Returns -1 if there is none.
 *
 * If `unbalanced` is non-NULL it is set when the scan ends inside a quoted
 * run.  That case matters: an unbalanced quote swallows the separator, so
 * without this the caller would report a missing colon when the real problem
 * is a missing quote -- a genuinely confusing thing to be told.
 */
static long find_split(const wchar_t *s, int *unbalanced)
{
    int in_quote = 0;
    size_t i;
    long found = -1;
    for (i = 0; s[i]; i++) {
        if (s[i] == L'"') { in_quote = !in_quote; continue; }
        if (in_quote) continue;
        if (s[i] == L':' && found < 0) {
            if (s[i + 1] == L':') { i++; continue; }
            found = (long)i;
        }
    }
    if (unbalanced) *unbalanced = in_quote;
    return found;
}

wchar_t *um_unwrap_spec(const wchar_t *spec)
{
    const wchar_t *s, *e;
    size_t len, i;
    int closed_at_end = 0;

    if (!spec) return wdup(L"");

    s = spec;
    while (*s && is_space(*s)) s++;
    e = s + wcslen(s);
    while (e > s && is_space(e[-1])) e--;
    len = (size_t)(e - s);

    if (len >= 2 && s[0] == L'"') {
        for (i = 1; i < len; i++) {
            if (s[i] != L'"') continue;
            if (i + 1 < len && s[i + 1] == L'"') { i++; continue; } /* "" */
            closed_at_end = (i == len - 1);
            break;
        }
        if (closed_at_end) return wdup_n(s + 1, len - 2);
    }
    return wdup_n(s, len);
}

static int all_space(const wchar_t *s)
{
    for (; *s; s++) if (!is_space(*s) && *s != L'\r' && *s != L'\n') return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public: command-line spec                                           */
/* ------------------------------------------------------------------ */

um_menu *um_parse_spec(const wchar_t *spec)
{
    um_menu *m = menu_new();
    wchar_t **names = NULL, **cmds = NULL;
    int nn = 0, nc = 0, i;
    int unterminated = 0;
    long split;

    if (!m) return NULL;

    if (!spec || all_space(spec))
        return menu_error(m, wdup(L"nothing to show: expected "
                                  L"\"name, name : command, command\""));

    split = find_split(spec, &unterminated);
    if (unterminated)
        return menu_error(m, wdup(L"unbalanced double quote -- every \" that "
                                  L"opens a quoted run needs one to close it "
                                  L"(write \"\" for a literal quote inside a "
                                  L"quoted field)"));
    if (split < 0)
        return menu_error(m, wdup(L"no ':' separator found -- names go before "
                                  L"the colon and commands after it, e.g. "
                                  L"um Go, Stop : go.cmd, stop.cmd"));

    if (!parse_fields(spec, (size_t)split, 1, &names, &nn, &unterminated))
        return menu_error(m, wdup(L"could not parse the names section "
                                  L"(out of memory, or more than 999 items)"));

    if (!parse_fields(spec + split + 1, wcslen(spec + split + 1), 0,
                      &cmds, &nc, &unterminated)) {
        for (i = 0; i < nn; i++) free(names[i]);
        free(names);
        return menu_error(m, wdup(L"could not parse the commands section "
                                  L"(out of memory, or more than 999 items)"));
    }

    if (unterminated) {
        menu_error(m, wdup(L"unbalanced double quote -- every \" that opens a "
                           L"quoted run needs one to close it (write \"\" for "
                           L"a literal quote inside a quoted field)"));
        goto cleanup;
    }

    if (nn != nc) {
        menu_error(m, wfmt(L"%d name%ls but %d command%ls -- each name needs "
                           L"exactly one command. For an item that does "
                           L"nothing, leave its command empty "
                           L"(e.g. um Go, Quit : go.cmd,)",
                           nn, nn == 1 ? L"" : L"s",
                           nc, nc == 1 ? L"" : L"s"));
        goto cleanup;
    }

    m->items = (um_item *)calloc((size_t)nn, sizeof(um_item));
    if (!m->items) {
        menu_error(m, wdup(L"out of memory"));
        goto cleanup;
    }

    for (i = 0; i < nn; i++) {
        m->items[i].name = names[i];
        m->items[i].cmd = cmds[i];
        m->items[i].is_separator = 0;
        names[i] = NULL;
        cmds[i] = NULL;
    }
    m->count = nn;

cleanup:
    for (i = 0; i < nn; i++) free(names[i]);
    for (i = 0; i < nc; i++) free(cmds[i]);
    free(names);
    free(cmds);
    return m;
}

/* ------------------------------------------------------------------ */
/* Public: menu files                                                  */
/* ------------------------------------------------------------------ */

static int line_is_separator(const wchar_t *s, size_t len)
{
    size_t i, dashes = 0;
    for (i = 0; i < len; i++) {
        if (s[i] == L'-') dashes++;
        else if (!is_space(s[i])) return 0;
    }
    return dashes >= 2;
}

static int menu_push(um_menu *m, int *cap, wchar_t *name, wchar_t *cmd, int sep)
{
    if (m->count + 1 > *cap) {
        int ncap = *cap ? *cap * 2 : 16;
        um_item *ni = (um_item *)realloc(m->items, (size_t)ncap * sizeof(um_item));
        if (!ni) return 0;
        m->items = ni;
        *cap = ncap;
    }
    m->items[m->count].name = name;
    m->items[m->count].cmd = cmd;
    m->items[m->count].is_separator = sep;
    m->count++;
    return 1;
}

um_menu *um_parse_file_text(const wchar_t *text)
{
    um_menu *m = menu_new();
    int cap = 0;
    long lineno = 0;
    const wchar_t *p;

    if (!m) return NULL;
    if (!text) return menu_error(m, wdup(L"menu file is empty"));

    p = text;
    while (*p || p == text) {
        const wchar_t *eol = p;
        size_t len, start = 0;
        long split;
        wchar_t *name, *cmd;
        size_t idx;

        while (*eol && *eol != L'\n' && *eol != L'\r') eol++;
        len = (size_t)(eol - p);
        lineno++;

        while (start < len && is_space(p[start])) start++;

        if (start == len) goto next;                       /* blank */
        if (p[start] == L'#' || p[start] == L';') goto next; /* comment */

        if (line_is_separator(p + start, len - start)) {
            if (!menu_push(m, &cap, wdup(L""), wdup(L""), 1))
                return menu_error(m, wdup(L"out of memory"));
            goto next;
        }

        if (p[start] == L'!') {
            /* Directives. Only !title so far. */
            const wchar_t *d = p + start + 1;
            size_t dlen = len - start - 1;
            if (dlen >= 5 && wcsncmp(d, L"title", 5) == 0 &&
                (dlen == 5 || is_space(d[5]))) {
                size_t k = 5;
                while (k < dlen && is_space(d[k])) k++;
                free(m->title);
                m->title = wdup_n(d + k, dlen - k);
            } else {
                return menu_error(m, wfmt(L"line %ld: unknown directive "
                                          L"(only !title is understood)",
                                          lineno));
            }
            goto next;
        }

        /* name : command, split on the first unquoted colon */
        {
            int unbalanced = 0;
            wchar_t *line = wdup_n(p + start, len - start);
            if (!line) return menu_error(m, wdup(L"out of memory"));
            split = find_split(line, &unbalanced);
            if (unbalanced) {
                wchar_t *e = wfmt(L"line %ld: unbalanced double quote", lineno);
                free(line);
                return menu_error(m, e);
            }
            if (split < 0) {
                wchar_t *e = wfmt(L"line %ld: no ':' -- a menu-file line looks "
                                  L"like  Name : command", lineno);
                free(line);
                return menu_error(m, e);
            }
            idx = 0;
            name = parse_one_field(line, (size_t)split, &idx, 1, 0, NULL);
            idx = 0;
            cmd = parse_one_field(line + split + 1, wcslen(line + split + 1),
                                  &idx, 0, 0, NULL);
            free(line);
            if (!name || !cmd) {
                free(name); free(cmd);
                return menu_error(m, wdup(L"out of memory"));
            }
            if (!menu_push(m, &cap, name, cmd, 0)) {
                free(name); free(cmd);
                return menu_error(m, wdup(L"out of memory"));
            }
            if (m->count > UM_MAX_ITEMS)
                return menu_error(m, wdup(L"too many menu items (limit 999)"));
        }

next:
        if (!*eol) break;
        if (eol[0] == L'\r' && eol[1] == L'\n') p = eol + 2;
        else p = eol + 1;
    }

    if (um_selectable_count(m) == 0)
        return menu_error(m, wdup(L"menu file contains no selectable items"));

    return m;
}

/* ------------------------------------------------------------------ */
/* Selectable-item bookkeeping                                         */
/* ------------------------------------------------------------------ */

int um_selectable_count(const um_menu *m)
{
    int i, n = 0;
    if (!m) return 0;
    for (i = 0; i < m->count; i++)
        if (!m->items[i].is_separator) n++;
    return n;
}

int um_selectable_index(const um_menu *m, int n)
{
    int i, k = 0;
    if (!m || n < 0) return -1;
    for (i = 0; i < m->count; i++) {
        if (m->items[i].is_separator) continue;
        if (k == n) return i;
        k++;
    }
    return -1;
}

wchar_t um_hotkey_for(int n)
{
    if (n < 0) return 0;
    if (n < 9) return (wchar_t)(L'1' + n);
    if (n < 9 + 26) return (wchar_t)(L'a' + (n - 9));
    return 0;
}

int um_hotkey_ordinal(wchar_t ch)
{
    if (ch >= L'1' && ch <= L'9') return (int)(ch - L'1');
    if (ch >= L'a' && ch <= L'z') return 9 + (int)(ch - L'a');
    if (ch >= L'A' && ch <= L'Z') return 9 + (int)(ch - L'A');
    return -1;
}
