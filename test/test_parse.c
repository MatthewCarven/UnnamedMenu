/*
 * test_parse.c -- parser test suite for `um`.
 *
 * Compiled and run natively on the build host (see test/run_tests.sh), because
 * um_parse.c is deliberately free of Win32 calls.  This lets the fiddly part
 * of the tool be exercised without a Windows machine in the loop.
 *
 *   cc -o test_parse test/test_parse.c src/um_parse.c -Isrc && ./test_parse
 */
#include "../src/um_parse.h"

#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static int tests_run = 0, tests_failed = 0;

static void fail(const char *what, const wchar_t *spec,
                 const wchar_t *expect, const wchar_t *got)
{
    tests_failed++;
    printf("  FAIL %s\n", what);
    printf("       spec:     %ls\n", spec ? spec : L"(null)");
    printf("       expected: [%ls]\n", expect ? expect : L"(null)");
    printf("       got:      [%ls]\n", got ? got : L"(null)");
}

/*
 * Check one spec.  `expect` is a flattened description of the expected parse:
 * "name|cmd" entries joined with "/".  A leading "!" means we expect an error.
 */
static void check(const char *what, const wchar_t *spec, const wchar_t *expect)
{
    um_menu *m = um_parse_spec(spec);
    wchar_t got[2048];
    int i;
    size_t n = 0;

    tests_run++;
    got[0] = L'\0';

    if (!m) { fail(what, spec, expect, L"(null menu)"); return; }

    if (m->error) {
        n += (size_t)swprintf(got + n, 2048 - n, L"!%ls", m->error);
    } else {
        for (i = 0; i < m->count; i++) {
            n += (size_t)swprintf(got + n, 2048 - n, L"%ls%ls|%ls",
                                  i ? L"/" : L"",
                                  m->items[i].name, m->items[i].cmd);
        }
    }

    /* For error cases we only require the "!" prefix plus a substring match,
       so error wording can be improved without breaking the suite. */
    if (expect[0] == L'!') {
        if (got[0] != L'!' || (expect[1] && !wcsstr(got, expect + 1)))
            fail(what, spec, expect, got);
    } else if (wcscmp(got, expect) != 0) {
        fail(what, spec, expect, got);
    }

    um_menu_free(m);
}

static void check_file(const char *what, const wchar_t *text,
                       const wchar_t *expect)
{
    um_menu *m = um_parse_file_text(text);
    wchar_t got[2048];
    int i;
    size_t n = 0;

    tests_run++;
    got[0] = L'\0';

    if (!m) { fail(what, text, expect, L"(null menu)"); return; }

    if (m->error) {
        n += (size_t)swprintf(got + n, 2048 - n, L"!%ls", m->error);
    } else {
        if (m->title)
            n += (size_t)swprintf(got + n, 2048 - n, L"T=%ls;", m->title);
        for (i = 0; i < m->count; i++) {
            if (m->items[i].is_separator)
                n += (size_t)swprintf(got + n, 2048 - n, L"%ls---",
                                      i ? L"/" : L"");
            else
                n += (size_t)swprintf(got + n, 2048 - n, L"%ls%ls|%ls",
                                      i ? L"/" : L"",
                                      m->items[i].name, m->items[i].cmd);
        }
    }

    if (expect[0] == L'!') {
        if (got[0] != L'!' || (expect[1] && !wcsstr(got, expect + 1)))
            fail(what, text, expect, got);
    } else if (wcscmp(got, expect) != 0) {
        fail(what, text, expect, got);
    }

    um_menu_free(m);
}

static void check_int(const char *what, long expect, long got)
{
    tests_run++;
    if (expect != got) {
        tests_failed++;
        printf("  FAIL %s: expected %ld, got %ld\n", what, expect, got);
    }
}

int main(void)
{
    setlocale(LC_ALL, "C.UTF-8");

    printf("um parser tests\n\n");

    printf("-- the original worked example --\n");
    /* As cmd would deliver it once %item_3% has been expanded away; and with
       the variable name intact, which is what a batch file escaping the
       percent signs would produce. */
    check("unquoted spec, spaces after commas",
          L"Item_1, Item_2, Item_3:item_1.cmd, item_2.exe, set %item_3%=0",
          L"Item_1|item_1.cmd/Item_2|item_2.exe/Item_3|set %item_3%=0");
    check("no spaces at all",
          L"A,B,C:a.cmd,b.exe,c.bat",
          L"A|a.cmd/B|b.exe/C|c.bat");
    check("generous spaces everywhere",
          L"  A ,  B  :  a.cmd ,  b.exe  ",
          L"A|a.cmd/B|b.exe");

    printf("-- the colon rule --\n");
    check("only the first colon splits; drive letters survive",
          L"Recover, Cancel : C:\\Windows\\System32\\diskpart.exe /s X:\\r.txt, exit",
          L"Recover|C:\\Windows\\System32\\diskpart.exe /s X:\\r.txt/Cancel|exit");
    check("URLs survive in commands",
          L"Docs : start http://example.com:8080/a",
          L"Docs|start http://example.com:8080/a");
    check(":: is a literal colon in a name",
          L"Step 1:: Wipe, Step 2:: Restore : wipe.cmd, restore.cmd",
          L"Step 1: Wipe|wipe.cmd/Step 2: Restore|restore.cmd");
    check("no colon at all is an error",
          L"A, B, C",
          L"!no ':' separator");

    printf("-- pairing --\n");
    check("count mismatch is an error, not a guess",
          L"A, B, C : a.cmd, b.cmd",
          L"!3 names but 2 commands");
    check("trailing comma gives an empty do-nothing command",
          L"Install, Quit : setup.exe,",
          L"Install|setup.exe/Quit|");
    check("single item",
          L"Only : only.exe",
          L"Only|only.exe");
    check("empty spec is an error",
          L"   ",
          L"!nothing to show");

    printf("-- quoting --\n");
    check("whole-field quotes are stripped and protect commas",
          L"\"Wipe, then restore\", Cancel : \"diskpart /s a.txt, imagex /apply b.wim\", exit",
          L"Wipe, then restore|diskpart /s a.txt, imagex /apply b.wim/Cancel|exit");
    check("mid-field quotes are KEPT and still protect commas",
          L"Open : notepad \"C:\\Users\\Admin\\My Notes, draft.txt\"",
          L"Open|notepad \"C:\\Users\\Admin\\My Notes, draft.txt\"");
    check("doubled quote inside a quoted field is one literal quote",
          L"Run : \"dir \"\"C:\\Program Files\"\"\"",
          L"Run|dir \"C:\\Program Files\"");
    check("whitespace inside quotes is preserved",
          L"\"  padded  \" : x.cmd",
          L"  padded  |x.cmd");
    check("explicit empty field via \"\"",
          L"A, \"\", B : x.cmd, y.cmd, z.cmd",
          L"A|x.cmd/|y.cmd/B|z.cmd");
    check("unbalanced quote is an error",
          L"A, \"B : x.cmd, y.cmd",
          L"!unbalanced double quote");
    check("a colon inside quotes does not split",
          L"\"Time: now\", B : a.cmd, b.cmd",
          L"Time: now|a.cmd/B|b.cmd");

    printf("-- doubling --\n");
    check(",, is a literal comma",
          L"Sort A,,Z, Cancel : \"dir /o:n\", exit",
          L"Sort A,Z|dir /o:n/Cancel|exit");
    check(",, in a command too",
          L"Echo : echo one,,two",
          L"Echo|echo one,two");

    printf("-- awkward but legal --\n");
    check("command containing an equals sign and percent signs",
          L"Set : set FOO=%BAR%",
          L"Set|set FOO=%BAR%");
    check("command with redirection and pipes passes through untouched",
          L"Log : dir /b > out.txt 2>&1 | more",
          L"Log|dir /b > out.txt 2>&1 | more");
    check("unicode names",
          L"Wipe \u2192 Restore, Abbrechen : go.cmd, exit",
          L"Wipe \u2192 Restore|go.cmd/Abbrechen|exit");
    check("empty name is allowed (odd, but not our business)",
          L", B : a.cmd, b.cmd",
          L"|a.cmd/B|b.cmd");

    printf("-- unwrapping a fully quoted spec --\n");
    {
        struct { const wchar_t *in, *out; const char *what; } cases[] = {
            { L"\"A, B : a.cmd, b.cmd\"", L"A, B : a.cmd, b.cmd",
              "whole spec quoted -> outer pair removed" },
            { L"A, B : a.cmd, b.cmd", L"A, B : a.cmd, b.cmd",
              "bare spec left alone" },
            { L"  \"A : a\"  ", L"A : a",
              "surrounding whitespace trimmed before and after" },
            { L"\"Wipe, then restore\", Cancel : go.cmd, exit",
              L"\"Wipe, then restore\", Cancel : go.cmd, exit",
              "quoted FIRST FIELD is not mistaken for a wrapped spec" },
            { L"\"A\" : \"a\"", L"\"A\" : \"a\"",
              "quoted first and last field, but not one pair -> left alone" },
            { L"A : notepad \"C:\\a b.txt\"", L"A : notepad \"C:\\a b.txt\"",
              "command ending in a quote is not a wrap" },
            { L"\"\"", L"", "empty quoted spec" },
            { L"", L"", "empty spec" },
        };
        size_t k;
        for (k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            wchar_t *got = um_unwrap_spec(cases[k].in);
            tests_run++;
            if (!got || wcscmp(got, cases[k].out) != 0)
                fail(cases[k].what, cases[k].in, cases[k].out, got);
            free(got);
        }
    }

    printf("-- menu files --\n");
    check_file("comments, blanks, title, separator",
               L"# a comment\n"
               L"!title Recovery Menu\n"
               L"\n"
               L"Restore image : X:\\restore.cmd\n"
               L"Partition     : diskpart /s X:\\wipe.txt\n"
               L"--------\n"
               L"Reboot        : wpeutil reboot\n",
               L"T=Recovery Menu;Restore image|X:\\restore.cmd/"
               L"Partition|diskpart /s X:\\wipe.txt/---/Reboot|wpeutil reboot");
    check_file("CRLF line endings",
               L"A : a.cmd\r\nB : b.cmd\r\n",
               L"A|a.cmd/B|b.cmd");
    check_file("no trailing newline",
               L"A : a.cmd",
               L"A|a.cmd");
    check_file("semicolon comments",
               L"; note\nA : a.cmd\n",
               L"A|a.cmd");
    check_file("commas are literal in a menu file",
               L"Wipe, then restore : diskpart /s a.txt, imagex /apply b.wim\n",
               L"Wipe, then restore|diskpart /s a.txt, imagex /apply b.wim");
    check_file("line with no colon is an error naming the line",
               L"A : a.cmd\noops\n",
               L"!line 2");
    check_file("unknown directive is an error",
               L"!banner hi\n",
               L"!unknown directive");
    check_file("a file of only separators has nothing to select",
               L"----\n----\n",
               L"!no selectable items");

    printf("-- hotkeys and selectable indexing --\n");
    check_int("hotkey 0 is '1'", L'1', um_hotkey_for(0));
    check_int("hotkey 8 is '9'", L'9', um_hotkey_for(8));
    check_int("hotkey 9 is 'a'", L'a', um_hotkey_for(9));
    check_int("hotkey 34 is 'z'", L'z', um_hotkey_for(34));
    check_int("hotkey 35 runs out", 0, um_hotkey_for(35));
    check_int("ordinal of '1'", 0, um_hotkey_ordinal(L'1'));
    check_int("ordinal of 'a'", 9, um_hotkey_ordinal(L'a'));
    check_int("ordinal of 'A' matches 'a'", 9, um_hotkey_ordinal(L'A'));
    check_int("ordinal of '0' is none", -1, um_hotkey_ordinal(L'0'));

    {
        /* Separators must not consume an index or a hotkey. */
        um_menu *m = um_parse_file_text(L"A : a\n----\nB : b\n");
        check_int("3 entries including the separator", 3, m ? m->count : -1);
        check_int("2 of them selectable", 2, um_selectable_count(m));
        check_int("selectable 0 is item 0", 0, um_selectable_index(m, 0));
        check_int("selectable 1 is item 2", 2, um_selectable_index(m, 1));
        check_int("selectable 2 does not exist", -1, um_selectable_index(m, 2));
        um_menu_free(m);
    }

    printf("\n%d tests, %d failed\n", tests_run, tests_failed);
    return tests_failed ? 1 : 0;
}
