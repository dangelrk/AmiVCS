/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_DIFF_H
#define CLOUDVCS_DIFF_H

/*
 * diff.h -- line diff of two texts: the common core of `diff`/`show`,
 * the GUIs' diff view and the line-level merge (merge3.h).
 *
 * Myers' O(ND) algorithm in its linear-space form (divide and conquer
 * on the "middle snake"), so memory stays proportional to the two texts
 * however different they are -- what matters on an Amiga with a few MB.
 * Common head and tail lines are cut off first. If a region needs more
 * than VCS_DIFF_MAX_D edits, it is reported as replaced as a whole
 * instead of searching on (the same trade as the WebUI's Diff.php).
 *
 * Lines point into the caller's buffer (no copy). A line includes its
 * end ("\n", "\r\n" or a lone "\r") so a merge reproduces the bytes
 * exactly; `eol` tells whether the line had one (the last line of a file
 * may not).
 */

#define VCS_DIFF_MAX_BYTES  (512L * 1024L) /* per side; larger = treated as binary */
#define VCS_DIFF_TEXT_PROBE 8192L          /* a NUL in the first bytes = binary */
#define VCS_DIFF_MAX_D      1500L
#define VCS_DIFF_CONTEXT    3

typedef struct {
    const char   *p;
    long          len;   /* bytes including the line end */
    long          tlen;  /* bytes without the line end */
    unsigned long hash;
} VcsLine;

typedef struct {
    VcsLine *items;
    long     n;
} VcsLines;

/* 1 if data looks like text: no NUL byte in the first VCS_DIFF_TEXT_PROBE
 * bytes and len <= VCS_DIFF_MAX_BYTES. */
int  vcs_diff_is_text(const void *data, long len);

/* Splits data into lines. Returns 1, 0 when out of memory. */
int  vcs_lines_split(const char *data, long len, VcsLines *out);
void vcs_lines_free(VcsLines *l);
/* 1 if both lines hold the same bytes (line end included). */
int  vcs_line_equal(const VcsLine *a, const VcsLine *b);

/* One run of the edit script: op ' ' (a[a..a+n) == b[b..b+n)),
 * '-' (a[a..a+n) deleted) or '+' (b[b..b+n) inserted). */
typedef struct {
    char op;
    long a, b, n;
} VcsDiffRun;

typedef struct {
    VcsDiffRun *items;
    int n, cap;
} VcsDiffScript;

/* Computes the edit script turning a into b. Within a change, deletions
 * come before insertions. Returns 1, 0 when out of memory. */
int  vcs_diff_lines(const VcsLines *a, const VcsLines *b, VcsDiffScript *out);
void vcs_diff_script_free(VcsDiffScript *s);

/* Hunk output (unified diff shape). Line numbers are 1-based; a side
 * with count 0 names the line before the hunk, as `diff -u` does. text
 * is the line without its end; eol 0 = the file ended without one. */
typedef struct {
    void (*hunk)(long a_start, long a_count, long b_start, long b_count, void *ctx);
    void (*line)(char op, const char *text, long len, int eol, void *ctx);
} VcsDiffSink;

/* Groups the script into hunks with `context` lines around each change
 * and feeds them to sink. Returns the number of hunks. */
int  vcs_diff_emit(const VcsLines *a, const VcsLines *b, const VcsDiffScript *s,
                   int context, const VcsDiffSink *sink, void *ctx);

/* Convenience: diff of two buffers straight into sink. Returns the
 * number of hunks, -1 when out of memory. */
int  vcs_diff_buffers(const char *a, long alen, const char *b, long blen,
                      int context, const VcsDiffSink *sink, void *ctx);

#endif /* CLOUDVCS_DIFF_H */
