/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * big.h - BigInt (progetto_ir.md § 11d): a ref value is a pointer to a
 * number of the run, 0 is the number 0 (memory at zero is a BigInt 0).
 * Counted as a str is (str.h): its count is the first word, and
 * meri_str_retain takes a reference on either. Values are immutable:
 * every function makes a new one. The arithmetic is mini-gmp
 * (src/third_party/mini-gmp), whose memory counts against the budget of
 * the run: past it, the trap NOMEM.
 */
#ifndef MERI_BIG_H
#define MERI_BIG_H

#include "vm/rt.h"

#include <stdint.h>

/* the functions big_* of runtime.def, print_big, str_from_big and
   str_to_big: 1 done (the result in a[0]), 0 the run stops (s->status
   says why), -1 not one of them */
int meri_big_call(meri_state *s, uint32_t id, uint64_t *a);
/* one reference less on the number of v; the last one frees it */
void meri_big_release(meri_state *s, uint64_t v);
/* the numbers of the run still alive, freed at its end */
uint64_t meri_big_alive(const meri_state *s);
void meri_big_free_all(meri_state *s);

#endif
