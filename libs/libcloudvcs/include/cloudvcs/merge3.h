/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_MERGE3_H
#define CLOUDVCS_MERGE3_H

/*
 * merge3.h -- line-level three-way merge of one text file (diff3), used
 * by merge.c when both sides changed the same text file.
 *
 * The two diffs base->ours and base->theirs (diff.h) give the base lines
 * both sides kept unchanged. Between two such lines lies a chunk: taken
 * from the side that changed it, taken once if both made the same change,
 * and a conflict if they changed it differently. A conflict is written
 * with git's markers, so editors and later tools recognise it:
 *
 *   <<<<<<< ours (main)
 *   ...our lines...
 *   =======
 *   ...their lines...
 *   >>>>>>> theirs (feature)
 *
 * Lines both sides share at the start or end of a conflicting chunk are
 * kept outside the markers. The marker lines end like the file's lines
 * (CRLF stays CRLF).
 */

/* Merges the three texts. ours_label/theirs_label follow the marker
 * ("ours (main)"), in the codeset the file is written in. *out is
 * malloc'd (caller frees), *nconflicts the number of conflict blocks.
 * Returns 1, 0 when out of memory. */
int vcs_merge3(const char *base, long blen, const char *ours, long olen,
               const char *theirs, long tlen,
               const char *ours_label, const char *theirs_label,
               char **out, long *outlen, int *nconflicts);

#endif /* CLOUDVCS_MERGE3_H */
