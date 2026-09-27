/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * str.h - the strings of a program: a length, the bytes and a NUL after
 * them (for str_ptr). A str value is a pointer to one, and 0 is "" in
 * memory and in registers alike (progetto_ir.md § 11c). First cut: a
 * string is never freed, so nothing is counted.
 */
#ifndef MERI_STR_H
#define MERI_STR_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t len;
    char data[];
} meri_str;

/* a new string of n bytes copied from s (s may be NULL for n = 0); NULL
   when memory is exhausted */
meri_str *meri_str_new(const char *s, size_t n);
/* a new string of n bytes, to be written by the caller */
meri_str *meri_str_alloc(size_t n);

/* the string a value holds: never NULL, 0 is the empty string */
static inline const meri_str *meri_str_of(uint64_t v)
{
    extern const meri_str meri_str_empty;
    return v ? (const meri_str *)(uintptr_t)v : &meri_str_empty;
}

static inline uint64_t meri_str_value(const meri_str *s)
{
    return (uint64_t)(uintptr_t)s;
}

#endif
