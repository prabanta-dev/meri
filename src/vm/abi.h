/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * abi.h - a call to a C function whose signature is known only at run
 * time, with no library for it: the calling convention is written here.
 *
 * The work is in two halves. The classification says where each value
 * goes (which register file, which register, the stack) and is ordinary
 * C in abi.c; a mistake there is silent (the callee reads wrong numbers),
 * so tests/test_abi.c checks it against functions the C compiler lays out
 * itself. The trampoline loads a frame that says what goes in which
 * register and what words go on the stack, calls, and brings back the
 * registers of a result: it knows nothing of the rules.
 *
 * Covered: the System V AMD64 ABI (Linux and the other x86-64 systems of
 * that ABI). Elsewhere meri_abi_available answers false with a reason.
 * Not covered by design: long double (on x86-64 it travels on the x87
 * stack), variadic functions, vectors and __int128.
 */
#ifndef MERI_ABI_H
#define MERI_ABI_H

#include <stdbool.h>
#include <stdint.h>

/* what a value is to a calling convention */
typedef enum {
    MERI_ABI_VOID,
    MERI_ABI_S8,
    MERI_ABI_U8,
    MERI_ABI_S16,
    MERI_ABI_U16,
    MERI_ABI_S32,
    MERI_ABI_U32,
    MERI_ABI_S64,
    MERI_ABI_U64,
    MERI_ABI_F32,
    MERI_ABI_F64,
    MERI_ABI_PTR,
    MERI_ABI_STRUCT,
} meri_abi_kind;

/* a type as the ABI sees it. A struct gives its fields and their offsets
   as its declaration laid them out (the caller's layout is believed, not
   derived again), its size and its alignment */
typedef struct meri_abi_type {
    meri_abi_kind kind;
    uint32_t size, align;
    uint32_t nfields;
    const struct meri_abi_type *const *fields;
    const uint32_t *offsets;
} meri_abi_type;

extern const meri_abi_type meri_abi_void, meri_abi_s8, meri_abi_u8,
    meri_abi_s16, meri_abi_u16, meri_abi_s32, meri_abi_u32, meri_abi_s64,
    meri_abi_u64, meri_abi_f32, meri_abi_f64, meri_abi_ptr;

/* true if calls can be made on this machine; else false and why */
bool meri_abi_available(const char **why);

/* the most arguments a call takes */
#define MERI_ABI_MAX_ARGS 64

/* call fn with n arguments of the types args[i], the bytes of each at
   vals[i] (as C lays the value out); the result, if any, into ret (room
   for its size). false, and why, when the call cannot be made: no
   calling convention here, too many arguments, a type the ABI does not
   carry (a struct with a field not at a multiple of its alignment, of
   size 0), memory exhausted */
bool meri_abi_call(void *fn, const meri_abi_type *result, uint32_t n,
                   const meri_abi_type *const *args, const void *const *vals,
                   void *ret, const char **why);

#endif
