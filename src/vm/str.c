/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * str.c - the strings of a program (str.h).
 */
#include "vm/str.h"

#include <stdlib.h>
#include <string.h>

/* the empty string, with its NUL: what the value 0 holds (a static
   flexible array member, an extension of GCC and Clang) */
const meri_str meri_str_empty = {0, {0}};

meri_str *meri_str_alloc(size_t n)
{
    meri_str *s;

    if (n > SIZE_MAX - sizeof(meri_str) - 1)
        return NULL;
    s = malloc(sizeof(meri_str) + n + 1);
    if (!s)
        return NULL;
    s->len = n;
    s->data[n] = 0;
    return s;
}

meri_str *meri_str_new(const char *s, size_t n)
{
    meri_str *x = meri_str_alloc(n);

    if (x && n)
        memcpy(x->data, s, n);
    return x;
}
