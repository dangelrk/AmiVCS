/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#include "varint.h"

size_t vcs_varint_write(unsigned char *buf, unsigned long v)
{
    size_t n = 0;
    while (v >= 0x80) {
        buf[n++] = (unsigned char)(v & 0x7f) | 0x80;
        v >>= 7;
    }
    buf[n++] = (unsigned char)v;
    return n;
}

size_t vcs_varint_read(const unsigned char *buf, size_t len, unsigned long *out)
{
    unsigned long v = 0;
    size_t n = 0;
    int shift = 0;

    while (1) {
        if (n >= len) return 0; /* truncated */
        {
            unsigned char byte = buf[n++];
            v |= (unsigned long)(byte & 0x7f) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
        }
    }
    *out = v;
    return n;
}
