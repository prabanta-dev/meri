/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ffi.h - the external (C) functions of a module (progetto_ir.md § 11e):
 * the libraries opened and every symbol resolved before the first
 * instruction runs, and call.ext through the calling convention of
 * abi.h. The module must follow the C of this platform (module.target);
 * meri_link (meri/meri.h) resolves it.
 */
#ifndef MERI_FFI_H
#define MERI_FFI_H

#include "vm/rt.h"
#include "vm/vm.h"

#include <stddef.h>

/* call extern x of p, its arguments in a[0..n) as the IR gives them (a
   struct by value as a ptr to its bytes; for a struct result, a[0] the
   ptr where it goes); the result, canonical, in a[0]. false when the
   run stops (s->status) */
bool meri_ffi_call(const meri_program *p, uint32_t x, uint64_t *a,
                   meri_state *s);
void meri_ffi_free(meri_program *p);

#endif
