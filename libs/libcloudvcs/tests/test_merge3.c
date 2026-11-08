/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * test_merge3.c -- merge3.c: clean merges, same change on both sides,
 * conflicts with git's markers, CRLF, missing final line end, add/add.
 */
#include "merge3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

static void expect(const char *name, const char *base, const char *ours, const char *theirs,
                   const char *want, int want_conflicts)
{
    char *out;
    long len;
    int nc;
    if (!vcs_merge3(base, (long)strlen(base), ours, (long)strlen(ours), theirs, (long)strlen(theirs),
                    "ours (main)", "theirs (feature)", &out, &len, &nc)) {
        printf("FAIL %s: out of memory\n", name);
        fails++;
        return;
    }
    if (nc != want_conflicts || (long)strlen(want) != len || memcmp(out, want, (size_t)len) != 0) {
        printf("FAIL %s: %d conflict(s), got:\n%.*s---\nwanted %d:\n%s---\n", name, nc, (int)len, out,
               want_conflicts, want);
        fails++;
    }
    free(out);
}

int main(void)
{
    expect("only ours", "a\nb\nc\n", "a\nB\nc\n", "a\nb\nc\n", "a\nB\nc\n", 0);
    expect("only theirs", "a\nb\nc\n", "a\nb\nc\n", "a\nb\nC\n", "a\nb\nC\n", 0);
    expect("both, different places", "1\n2\n3\n4\n5\n", "1\nX\n3\n4\n5\n", "1\n2\n3\n4\nY\n",
           "1\nX\n3\n4\nY\n", 0);
    expect("same change", "a\nb\nc\n", "a\nB\nc\n", "a\nB\nc\n", "a\nB\nc\n", 0);
    expect("insert on both ends", "m\n", "top\nm\n", "m\nend\n", "top\nm\nend\n", 0);
    expect("delete vs edit elsewhere", "a\nb\nc\nd\n", "a\nc\nd\n", "a\nb\nc\nD\n", "a\nc\nD\n", 0);
    expect("conflict", "a\nb\nc\n", "a\nours\nc\n", "a\ntheirs\nc\n",
           "a\n<<<<<<< ours (main)\nours\n=======\ntheirs\n>>>>>>> theirs (feature)\nc\n", 1);
    expect("conflict, shared edge lines outside", "a\nb\nc\n", "a\nX\nb1\nY\nc\n", "a\nX\nb2\nY\nc\n",
           "a\nX\n<<<<<<< ours (main)\nb1\n=======\nb2\n>>>>>>> theirs (feature)\nY\nc\n", 1);
    expect("crlf", "a\r\nb\r\nc\r\n", "a\r\nO\r\nc\r\n", "a\r\nT\r\nc\r\n",
           "a\r\n<<<<<<< ours (main)\r\nO\r\n=======\r\nT\r\n>>>>>>> theirs (feature)\r\nc\r\n", 1);
    expect("no final line end", "a\nb", "a\nO", "a\nT",
           "a\n<<<<<<< ours (main)\nO\n=======\nT\n>>>>>>> theirs (feature)\n", 1);
    expect("add/add (empty base), same", "", "x\n", "x\n", "x\n", 0);
    expect("add/add (empty base), different", "", "x\n", "y\n",
           "<<<<<<< ours (main)\nx\n=======\ny\n>>>>>>> theirs (feature)\n", 1);
    expect("two conflicts", "1\n2\n3\n4\n5\n", "1\nA\n3\n4\nB\n", "1\nC\n3\n4\nD\n",
           "1\n<<<<<<< ours (main)\nA\n=======\nC\n>>>>>>> theirs (feature)\n3\n4\n"
           "<<<<<<< ours (main)\nB\n=======\nD\n>>>>>>> theirs (feature)\n", 2);
    expect("empty everything", "", "", "", "", 0);
    expect("both delete everything differently", "a\nb\n", "", "a\n",
           "<<<<<<< ours (main)\n=======\na\n>>>>>>> theirs (feature)\n", 1);

    if (fails) { printf("test_merge3: %d failure(s)\n", fails); return 1; }
    printf("test_merge3: ok\n");
    return 0;
}
