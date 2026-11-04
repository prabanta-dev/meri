/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * files.h - the files of a run (doc/ir.md of Limba § 15.11): file_open ...
 * file_delete of runtime.def, and pos_str.
 *
 * A handle is the number of the opening that succeeded in the run, from 1,
 * never given again; at most MERI_FILES_MAX are open at once. Each has a
 * buffer of MERI_FILE_BUF bytes, written with write(2) only when full, at
 * file_flush and at file_close (never a FILE of the C library, whose own
 * buffer would move the call that finds an error of writing), and filled
 * with read(2) for a file read. The buffers are the run time's memory,
 * outside the budget of the program and the strings counted, as in the
 * reference interpreter.
 */
#ifndef MERI_FILES_H
#define MERI_FILES_H

#include "vm/rt.h"

#include <stdbool.h>
#include <stdint.h>

#define MERI_FILES_MAX 256
#define MERI_FILE_BUF 65536

typedef struct {
    int64_t handle; /* 0: a free entry */
    int fd;
    int mode;       /* 0 read, 1 write, 2 append */
    bool end;       /* read: the end met, for good */
    size_t len, at; /* the bytes in buf; read: the next one */
    uint8_t *buf;
} meri_file;

struct meri_files {
    int64_t next;  /* the handle of the next opening */
    meri_file *at; /* the entry found last */
    meri_file f[MERI_FILES_MAX];
};

/* pos_str and the files: 1 done (the result in a[0]), 0 the run stops
   (s->status says why; the words of a trap IO made by the call in
   s->trap_msg), -1 not one of them */
int meri_files_call(meri_state *s, uint32_t id, uint64_t *a);

/* at the end of a run, whatever the end: every file still open written
   and closed, the table freed. false if writing or closing one failed */
bool meri_files_end(meri_state *s);

#endif
