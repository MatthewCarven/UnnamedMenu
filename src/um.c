/*
 * um.c -- entry point, option parsing and dispatch for `um`.
 *
 * The important structural decision lives here: `um` never looks at argv.
 * It takes GetCommandLineW(), steps over its own program name, walks the
 * options off the front itself, and hands the entire untouched remainder to
 * the parser as one string.
 *
 * That is what makes the original design goal work unquoted:
 *
 *     um Item_1, Item_2, Item_3:item_1.cmd, item_2.exe, set %item_3%=0
 *
 * The C runtime would have split that on spaces and destroyed the spacing
 * inside `set %item_3%=0`. See docs/SYNTAX.md section 2.
 */
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "um_parse.h"
#include "um_win.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Option state                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    wchar_t *title;
    wchar_t *file;
    int  default_sel;    /* 1-based as typed; -1 = not given */
    int  wait_secs;
    int  emit;
    int  emit_name;
    int  exit_index;
    int  instant;
    int  want_help;
    int  want_version;
    int  dump;
    wchar_t *error;
} um_opts;

static const wchar_t *USAGE =
L"um " UM_VERSION L" -- a command-line menu that takes everything from the\r\n"
L"command line.\r\n"
L"\r\n"
L"USAGE\r\n"
L"  um [options] <names> : <commands>\r\n"
L"  um [options] -f <menufile>\r\n"
L"\r\n"
L"  Names and commands are comma-separated and paired up in order. The first\r\n"
L"  colon splits the two groups, so colons after it -- drive letters, URLs --\r\n"
L"  need no escaping.\r\n"
L"\r\n"
L"    um Item_1, Item_2, Item_3 : item_1.cmd, item_2.exe, set FOO=0\r\n"
L"\r\n"
L"OPTIONS\r\n"
L"  -t, --title TEXT     heading drawn above the menu\r\n"
L"  -d, --default N      item highlighted at startup, and picked on timeout\r\n"
L"  -w, --wait SECS      auto-pick the default after SECS with no keypress\r\n"
L"  -e, --emit           print the chosen COMMAND to stdout; run nothing\r\n"
L"  -n, --name           print the chosen NAME to stdout; run nothing\r\n"
L"  -f, --file PATH      read the menu from a file, one \"Name : cmd\" per line\r\n"
L"  -x, --exit-index     exit with the selection index, not the command's code\r\n"
L"  -1, --instant        a hotkey selects immediately, without Enter\r\n"
L"      --dump           show how the spec was parsed, then stop\r\n"
L"  -h, --help           this text\r\n"
L"  -V, --version        version\r\n"
L"  --                   stop reading options (use it if a name starts with -)\r\n"
L"\r\n"
L"KEYS\r\n"
L"  Up/Down move    Home/End jump to ends    1-9 a-z jump to an item\r\n"
L"  Enter select    Esc or Ctrl+C cancel\r\n"
L"\r\n"
L"SETTING VARIABLES IN THE CALLING SHELL\r\n"
L"  A child process cannot change its parent's environment, so `um` cannot\r\n"
L"  simply run `set FOO=1` for you. Use -e and let the shell run it instead:\r\n"
L"\r\n"
L"    for /f \"delims=\" %%c in ('um -e \"Dev, Live : set ENV=dev, set ENV=live\"') do %%c\r\n"
L"\r\n"
L"  (One % instead of two when typing that straight at the prompt.)\r\n"
L"  The menu still draws on screen -- it is written to the console device, not\r\n"
L"  to stdout, so the pipe carries only the chosen command.\r\n"
L"\r\n"
L"EXIT CODES\r\n"
L"  run mode   the command's own exit code\r\n"
L"  1..N       the 1-based index chosen (with -e, -n, -x, or an empty command)\r\n"
L"  0          cancelled, or timed out with no default\r\n"
L"  255        usage or parse error; nothing ran\r\n";

/* ------------------------------------------------------------------ */
/* Raw command-line tokenising                                         */
/* ------------------------------------------------------------------ */

static int is_space(wchar_t c) { return c == L' ' || c == L'\t'; }

/* Step over argv[0] in the raw command line, quoted or not. */
static const wchar_t *skip_program_name(const wchar_t *p)
{
    if (!p) return L"";
    while (is_space(*p)) p++;
    if (*p == L'"') {
        p++;
        while (*p && *p != L'"') p++;
        if (*p == L'"') p++;
    } else {
        while (*p && !is_space(*p)) p++;
    }
    while (is_space(*p)) p++;
    return p;
}

/*
 * Pull one whitespace-delimited token, honouring double quotes so that
 * -t "My Title" works. Quotes group and are stripped; "" inside a quoted run
 * is a literal quote. Returns an allocated string, or NULL at end of input.
 */
static wchar_t *next_token(const wchar_t **pp)
{
    const wchar_t *p = *pp;
    wchar_t *out;
    size_t n = 0, cap;
    int in_quote = 0;

    while (is_space(*p)) p++;
    if (!*p) { *pp = p; return NULL; }

    cap = wcslen(p) + 1;
    out = (wchar_t *)malloc(cap * sizeof(wchar_t));
    if (!out) { *pp = p; return NULL; }

    while (*p) {
        if (*p == L'"') {
            if (in_quote && p[1] == L'"') { out[n++] = L'"'; p += 2; continue; }
            in_quote = !in_quote;
            p++;
            continue;
        }
        if (!in_quote && is_space(*p)) break;
        out[n++] = *p++;
    }
    out[n] = L'\0';
    *pp = p;
    return out;
}

/* ------------------------------------------------------------------ */
/* Option parsing                                                      */
/* ------------------------------------------------------------------ */

static int parse_int(const wchar_t *s, int *out)
{
    wchar_t *end;
    long v;
    if (!s || !*s) return 0;
    v = wcstol(s, &end, 10);
    while (end && is_space(*end)) end++;
    if (!end || *end) return 0;
    if (v < 0 || v > 100000) return 0;
    *out = (int)v;
    return 1;
}

/*
 * Walk options off the front of `tail`. Returns a pointer into `tail` at the
 * first thing that is not an option -- that is the spec, and it is used raw
 * from there to the end of the string.
 */
static const wchar_t *parse_options(const wchar_t *tail, um_opts *o)
{
    const wchar_t *p = tail;

    memset(o, 0, sizeof *o);
    o->default_sel = -1;

    for (;;) {
        const wchar_t *before;
        wchar_t *tok, *name, *inlineval = NULL;
        int is_long, needs_value = 0;
        wchar_t **value_target = NULL;
        int *int_target = NULL;

        while (is_space(*p)) p++;
        if (!*p) return p;
        if (*p != L'-' && *p != L'/') return p;      /* spec starts here */

        before = p;
        tok = next_token(&p);
        if (!tok) return before;

        if (wcscmp(tok, L"--") == 0) {               /* end of options */
            free(tok);
            while (is_space(*p)) p++;
            return p;
        }
        if (wcscmp(tok, L"/?") == 0 || wcscmp(tok, L"-?") == 0) {
            o->want_help = 1; free(tok); continue;
        }
        /* A lone "-" or "/" is not an option; treat it as the spec. */
        if (tok[1] == L'\0') { free(tok); return before; }

        is_long = (tok[0] == L'-' && tok[1] == L'-');
        name = is_long ? tok + 2 : tok + 1;

        if (is_long) {
            wchar_t *eq = wcschr(name, L'=');
            if (eq) { *eq = L'\0'; inlineval = eq + 1; }
        }

/* Two-level widening so the L prefix is applied after macro expansion --
   the one-level form is implementation-defined and MSVC is fussier than GCC. */
#define UM_WIDEN2(x) L##x
#define UM_WIDEN(x)  UM_WIDEN2(x)
#define LONG_IS(s)  (is_long && wcscmp(name, UM_WIDEN(s)) == 0)
#define SHORT_IS(c) (!is_long && name[0] == UM_WIDEN(c))

        if (LONG_IS("help") || (SHORT_IS('h') && !name[1])) {
            o->want_help = 1;
        } else if (LONG_IS("version") || (SHORT_IS('V') && !name[1])) {
            o->want_version = 1;
        } else if (LONG_IS("dump")) {
            o->dump = 1;
        } else if (LONG_IS("emit") || (SHORT_IS('e') && !name[1])) {
            o->emit = 1;
        } else if (LONG_IS("name") || (SHORT_IS('n') && !name[1])) {
            o->emit_name = 1;
        } else if (LONG_IS("exit-index") || (SHORT_IS('x') && !name[1])) {
            o->exit_index = 1;
        } else if (LONG_IS("instant") || (SHORT_IS('1') && !name[1])) {
            o->instant = 1;
        } else if (LONG_IS("title") || SHORT_IS('t')) {
            needs_value = 1; value_target = &o->title;
        } else if (LONG_IS("file") || SHORT_IS('f')) {
            needs_value = 1; value_target = &o->file;
        } else if (LONG_IS("default") || SHORT_IS('d')) {
            needs_value = 1; int_target = &o->default_sel;
        } else if (LONG_IS("wait") || SHORT_IS('w')) {
            needs_value = 1; int_target = &o->wait_secs;
        } else {
            o->error = _wcsdup(L"unknown option -- run um -h for the list");
            free(tok);
            return p;
        }

#undef LONG_IS
#undef SHORT_IS
#undef UM_WIDEN
#undef UM_WIDEN2

        if (needs_value) {
            wchar_t *val = NULL;
            if (inlineval) {
                val = _wcsdup(inlineval);            /* --title=X */
            } else if (!is_long && name[1]) {
                val = _wcsdup(name + 1);             /* -tX */
            } else {
                val = next_token(&p);                /* -t X */
            }
            if (!val || !*val) {
                free(val);
                o->error = _wcsdup(L"an option is missing its value");
                free(tok);
                return p;
            }
            if (value_target) {
                free(*value_target);
                *value_target = val;
            } else if (int_target) {
                if (!parse_int(val, int_target))
                    o->error = _wcsdup(L"expected a number after -d or -w");
                free(val);
            }
        }

        free(tok);
        if (o->error) return p;
    }
}

static void opts_free(um_opts *o)
{
    free(o->title);
    free(o->file);
    free(o->error);
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

/* Which selectable ordinal (0-based) is this item index? -1 if a separator. */
static int ordinal_of(const um_menu *m, int item_index)
{
    int i, k = 0;
    for (i = 0; i < m->count; i++) {
        if (m->items[i].is_separator) continue;
        if (i == item_index) return k;
        k++;
    }
    return -1;
}

static void dump_menu(const um_menu *m, const um_opts *o)
{
    wchar_t line[2048];
    int i, ord = 0;

    um_emit(L"um " UM_VERSION L" -- parse dump");
    swprintf(line, 2048, L"raw line:  %ls", GetCommandLineW());
    um_emit(line);
    swprintf(line, 2048, L"title:      %ls", o->title ? o->title :
             (m->title ? m->title : L"(none)"));
    um_emit(line);
    swprintf(line, 2048, L"entries:    %d  (%d selectable)",
             m->count, um_selectable_count(m));
    um_emit(line);
    um_emit(L"");
    um_emit(L"Angle brackets show exactly where each field starts and ends,");
    um_emit(L"so you can see what quoting did.");
    um_emit(L"");

    for (i = 0; i < m->count; i++) {
        if (m->items[i].is_separator) {
            um_emit(L"      ---- separator ----");
            continue;
        }
        swprintf(line, 2048, L"  [%lc] name=<%ls>",
                 um_hotkey_for(ord) ? um_hotkey_for(ord) : L'?',
                 m->items[i].name);
        um_emit(line);
        swprintf(line, 2048, L"       cmd=<%ls>", m->items[i].cmd);
        um_emit(line);
        ord++;
    }
}

static void fail(const wchar_t *msg)
{
    wchar_t line[1024];
    swprintf(line, 1024, L"um: %ls", msg);
    um_err(line);
    um_err(L"     (um -h for usage, um --dump ... to see how a spec parses)");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
    um_opts o;
    um_menu *m = NULL;
    um_ui_opts ui;
    const wchar_t *tail;
    wchar_t *spec;
    int chosen, ord, rc = 0;
    int selectable;

    tail = skip_program_name(GetCommandLineW());
    /* Everything from the first non-option token to the end of the line is
       the spec, used raw -- except that a single pair of quotes around the
       whole thing is removed, since that is how people naturally write it. */
    spec = um_unwrap_spec(parse_options(tail, &o));

    if (o.want_help)    { um_emit(USAGE); free(spec); opts_free(&o); return 0; }
    if (o.want_version) { um_emit(L"um " UM_VERSION); free(spec); opts_free(&o); return 0; }

    if (o.error) { fail(o.error); free(spec); opts_free(&o); return 255; }

    if (o.emit && o.emit_name) {
        fail(L"-e and -n do different things; pick one");
        free(spec);
        opts_free(&o);
        return 255;
    }

    /* --- build the menu --- */
    if (o.file) {
        wchar_t *err = NULL, *text;
        if (spec && *spec) {
            fail(L"a menu file and an inline menu were both given -- pick one");
            free(spec);
            opts_free(&o);
            return 255;
        }
        text = um_read_file_text(o.file, &err);
        if (!text) {
            fail(err ? err : L"could not read the menu file");
            free(err);
            free(spec);
            opts_free(&o);
            return 255;
        }
        m = um_parse_file_text(text);
        free(text);
    } else {
        if (!spec || !*spec) {
            um_emit(USAGE);
            free(spec);
            opts_free(&o);
            return 255;
        }
        m = um_parse_spec(spec);
    }
    free(spec);
    spec = NULL;

    if (!m) { fail(L"out of memory"); opts_free(&o); return 255; }
    if (m->error) {
        fail(m->error);
        um_menu_free(m);
        opts_free(&o);
        return 255;
    }

    selectable = um_selectable_count(m);
    if (selectable == 0) {
        fail(L"the menu has no selectable items");
        um_menu_free(m);
        opts_free(&o);
        return 255;
    }

    if (o.default_sel >= 0) {
        if (o.default_sel < 1 || o.default_sel > selectable) {
            wchar_t line[256];
            swprintf(line, 256,
                     L"-d %d is out of range -- there %ls %d item%ls",
                     o.default_sel, selectable == 1 ? L"is" : L"are",
                     selectable, selectable == 1 ? L"" : L"s");
            fail(line);
            um_menu_free(m);
            opts_free(&o);
            return 255;
        }
        o.default_sel -= 1;                            /* to 0-based ordinal */
    }

    if (o.dump) {
        dump_menu(m, &o);
        um_menu_free(m);
        opts_free(&o);
        return 0;
    }

    /* --- show it --- */
    ui.title = o.title ? o.title : m->title;
    ui.default_sel = o.default_sel;
    ui.wait_secs = o.wait_secs;
    ui.instant = o.instant;

    chosen = um_run_menu(m, &ui);

    if (chosen < 0) {                                  /* cancelled */
        um_menu_free(m);
        opts_free(&o);
        return 0;
    }

    ord = ordinal_of(m, chosen);
    if (ord < 0) ord = 0;

    if (o.emit) {
        um_emit(m->items[chosen].cmd);
        rc = ord + 1;
    } else if (o.emit_name) {
        um_emit(m->items[chosen].name);
        rc = ord + 1;
    } else if (!m->items[chosen].cmd || !m->items[chosen].cmd[0]) {
        rc = ord + 1;                                  /* nothing to run */
    } else {
        int code = um_exec(m->items[chosen].cmd);
        rc = o.exit_index ? ord + 1 : code;
    }

    um_menu_free(m);
    opts_free(&o);
    return rc;
}
