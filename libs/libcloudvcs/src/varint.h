/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef CLOUDVCS_VARINT_H
#define CLOUDVCS_VARINT_H

/*
 * varint.h -- private LEB128-style unsigned varint encoding for object.c's
 * binary Tree/Commit payloads. Not part of libcloudvcs's public API
 * (include/cloudvcs/): this is internal serialization plumbing, kept out
 * of the public headers on purpose (docs/adr/09_ADR_VCS_Design.md's "no
 * functionality implemented twice" cuts both ways -- no premature
 * abstraction either, see the plan's section 6).
 */

#include <stddef.h>

/* Appends v to buf as a LEB128 varint (buf must have room for at least 10
 * bytes -- enough for any 64-bit value). Returns the number of bytes
 * written. */
size_t vcs_varint_write(unsigned char *buf, unsigned long v);

/* Reads a varint starting at buf[0..len). On success returns the number
 * of bytes consumed and fills *out; returns 0 if the buffer runs out
 * before a terminating byte (truncated input). */
size_t vcs_varint_read(const unsigned char *buf, size_t len, unsigned long *out);

#endif /* CLOUDVCS_VARINT_H */
