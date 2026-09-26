/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * summary.h - a short account of a module of the IR: what it holds and how
 * big each function is.
 */
#ifndef MERI_SUMMARY_H
#define MERI_SUMMARY_H

#include "limba/ir.h"

#include <stdio.h>

void meri_summary(const limba_module *m, FILE *out);

#endif
