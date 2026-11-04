/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * files.c - the files of a run and pos_str (files.h), as the reference
 * interpreter of Limba runs them: open(2), read(2), write(2), close(2),
 * with buffers of their own. The arguments the IR checks before (the
 * tract of file_read and file_write, the mode) come first, as a wrong
 * module, then the handle.
 */
#include "vm/files.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum { OK, HANDLE, IO, NOTFOUND, DENIED, CANNOT, TOOMANY, NOMEM };

/* the table of the run, made at the first call that needs it */
static struct meri_files *table(meri_state *s)
{
    if (!s->files) {
        s->files = calloc(1, sizeof(*s->files));
        if (s->files)
            s->files->next = 1;
    }
    return s->files;
}

static meri_file *find(meri_state *s, int64_t handle)
{
    struct meri_files *t = s->files;
    int i;

    if (!t || handle <= 0 || handle >= t->next)
        return NULL;
    if (t->at && t->at->handle == handle)
        return t->at;
    for (i = 0; i < MERI_FILES_MAX; i++)
        if (t->f[i].handle == handle)
            return t->at = &t->f[i];
    return NULL;
}

/* why a name could not be opened or removed */
static int why(int err)
{
    switch (err) {
    case ENOENT:
    case ENOTDIR:
        return NOTFOUND;
    case EACCES:
    case EPERM:
    case EROFS:
        return DENIED;
    default:
        return CANNOT;
    }
}

static int open_file(meri_state *s, const char *name, int mode, int64_t *h)
{
    static const int how[3] = {O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC,
                               O_WRONLY | O_CREAT | O_APPEND};
    struct meri_files *t = table(s);
    meri_file *f = NULL;
    struct stat st;
    uint8_t *buf;
    int fd, i;

    if (!t)
        return NOMEM;
    for (i = 0; i < MERI_FILES_MAX && !f; i++)
        if (!t->f[i].handle)
            f = &t->f[i];
    if (!f)
        return TOOMANY;
    do
        fd = open(name, how[mode] | O_CLOEXEC, 0666);
    while (fd < 0 && errno == EINTR);
    if (fd < 0)
        return why(errno);
    if (fstat(fd, &st) || S_ISDIR(st.st_mode)) {
        close(fd);
        return CANNOT;
    }
    if (!(buf = malloc(MERI_FILE_BUF))) {
        close(fd);
        return NOMEM;
    }
    *f = (meri_file){.handle = t->next++, .fd = fd, .mode = mode, .buf = buf};
    *h = f->handle;
    return OK;
}

/* the n bytes from p written out, all of them */
static bool put(int fd, const uint8_t *p, size_t n)
{
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

/* the buffer of a file written out */
static int drain(meri_file *f)
{
    bool ok;

    if (!f->mode || !f->len)
        return OK;
    ok = put(f->fd, f->buf, f->len);
    f->len = 0;
    return ok ? OK : IO;
}

static int shut(struct meri_files *t, meri_file *f)
{
    int r = drain(f);

    if (close(f->fd) && r == OK && errno != EINTR)
        r = IO;
    free(f->buf);
    if (t->at == f)
        t->at = NULL;
    *f = (meri_file){0};
    return r;
}

/* n bytes from p (the program's) into the buffer of the file */
MERI_TOUCHES_PROGRAM static int write_file(meri_file *f, const uint8_t *p,
                                           size_t n)
{
    if (!f->mode)
        return IO;
    while (n) {
        size_t k = MERI_FILE_BUF - f->len < n ? MERI_FILE_BUF - f->len : n;
        memcpy(f->buf + f->len, p, k);
        f->len += k;
        p += k;
        n -= k;
        if (f->len == MERI_FILE_BUF && drain(f) != OK)
            return IO;
    }
    return OK;
}

/* more bytes into the buffer of a file read: false at an error; f->end
   at its end */
static bool fill(meri_file *f)
{
    ssize_t r;

    if (f->end)
        return true;
    if (f->at == f->len)
        f->at = f->len = 0;
    do
        r = read(f->fd, f->buf + f->len, MERI_FILE_BUF - f->len);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return false;
    if (r == 0)
        f->end = true;
    f->len += (size_t)r;
    return true;
}

/* up to n bytes into p (the program's): fewer only at the end */
MERI_TOUCHES_PROGRAM static int read_file(meri_file *f, uint8_t *p, size_t n,
                                          size_t *got)
{
    *got = 0;
    if (f->mode)
        return IO;
    while (*got < n) {
        size_t k;
        if (f->at == f->len) {
            if (f->end)
                break;
            if (!fill(f))
                return IO;
            continue;
        }
        k = f->len - f->at < n - *got ? f->len - f->at : n - *got;
        memcpy(p + *got, f->buf + f->at, k);
        f->at += k;
        *got += k;
    }
    return OK;
}

/* a line without its LF or CR LF, malloc'd in *line (NULL for none);
 *got false at the end */
static int read_line(meri_file *f, char **line, size_t *len, bool *got)
{
    size_t n = 0, cap = 0;
    char *b = NULL;
    bool any = false;

    *line = NULL;
    *len = 0;
    *got = false;
    if (f->mode)
        return IO;
    for (;;) {
        uint8_t c;
        if (f->at == f->len) {
            if (f->end)
                break;
            if (f->len == MERI_FILE_BUF) /* all read: room for more */
                f->at = f->len = 0;
            if (!fill(f)) {
                free(b);
                return IO;
            }
            continue;
        }
        c = f->buf[f->at++];
        any = true;
        if (c == '\n')
            break;
        if (n == cap) {
            char *nb =
                cap > SIZE_MAX / 2 ? NULL : realloc(b, cap ? 2 * cap : 64);
            if (!nb) {
                free(b);
                return NOMEM;
            }
            b = nb;
            cap = cap ? 2 * cap : 64;
        }
        b[n++] = (char)c;
    }
    if (!any) {
        free(b);
        return OK; /* the end */
    }
    if (n && b[n - 1] == '\r')
        n--;
    *line = b;
    *len = n;
    *got = true;
    return OK;
}

/* the trap IO with words the call makes: what, then n bytes of name, a
   str of the run in the budget, as its result would be */
static int trap_said(meri_state *s, const char *what, const char *name,
                     size_t n)
{
    size_t w = strlen(what);
    meri_str *x = meri_state_alloc(s, w + n);

    if (!x)
        return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
    memcpy(x->data, what, w);
    if (n)
        memcpy(x->data + w, name, n);
    x->data[w + n] = 0;
    s->trap_msg = meri_str_value(x);
    return meri_rt_trap(s, LIMBA_TRAP_IO);
}

/* the end of a call: 1, or its trap */
static int done(meri_state *s, int res, const meri_str *name, bool deleting)
{
    switch (res) {
    case OK:
        return 1;
    case HANDLE:
        return meri_rt_trap(s, LIMBA_TRAP_INVALID_HANDLE);
    case NOTFOUND:
        return trap_said(s, "not found: ", name->data, name->len);
    case DENIED:
        return trap_said(s, "not allowed: ", name->data, name->len);
    case CANNOT:
        return trap_said(s, deleting ? "cannot delete: " : "cannot open: ",
                         name->data, name->len);
    case TOOMANY:
        return trap_said(s, "too many open files", "", 0);
    case NOMEM:
        return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
    default:
        return meri_rt_trap(s, LIMBA_TRAP_IO);
    }
}

/* a wrong module: a check the IR owes the call is missing */
static int wrong(meri_state *s)
{
    s->status = MERI_BADMEM;
    return 0;
}

/* store the str v at p (the program's), as read_line: the old one
   released */
MERI_TOUCHES_PROGRAM static void store_str(meri_state *s, uint64_t p,
                                           uint64_t v)
{
    void *q = (void *)(uintptr_t)MERI_ADDR(p);
    uint64_t old;

    memcpy(&old, q, sizeof(old));
    memcpy(q, &v, sizeof(v));
    meri_state_release(s, old);
}

static int names(meri_state *s, uint32_t id, uint64_t *a)
{
    const meri_str *name = meri_str_of(a[0]);
    int32_t mode = (int32_t)a[1];
    int64_t h = 0;
    int res = OK;
    char *z;

    if (id == LIMBA_RT_FILE_OPEN && (mode < 0 || mode > 2))
        return wrong(s);
    if (memchr(name->data, 0, name->len))
        return meri_rt_trap(s, LIMBA_TRAP_RANGE); /* the system would cut it */
    if (!(z = malloc(name->len + 1)))
        return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
    memcpy(z, name->data, name->len);
    z[name->len] = 0;
    if (id == LIMBA_RT_FILE_OPEN) {
        res = open_file(s, z, mode, &h);
    } else if (id == LIMBA_RT_FILE_DELETE) {
        res = unlink(z) ? why(errno) : OK;
    } else {
        struct stat st;
        h = stat(z, &st) == 0;
    }
    free(z);
    a[0] = (uint64_t)h;
    return done(s, res, name, id == LIMBA_RT_FILE_DELETE);
}

int meri_files_call(meri_state *s, uint32_t id, uint64_t *a)
{
    meri_file *f;

    switch (id) {
    case LIMBA_RT_POS_STR: { /* k read without a sign */
        uint32_t k = (uint32_t)a[0];
        size_t n = limba_pos_print(s->p->m, k, NULL, 0);
        meri_str *x = meri_state_alloc(s, n);
        if (!x)
            return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
        if (n)
            limba_pos_print(s->p->m, k, x->data, n + 1);
        x->data[n] = 0;
        a[0] = meri_str_value(x);
        return 1;
    }
    case LIMBA_RT_FILE_OPEN:
    case LIMBA_RT_FILE_EXISTS:
    case LIMBA_RT_FILE_DELETE:
        return names(s, id, a);
    case LIMBA_RT_FILE_CLOSE:
        f = find(s, (int64_t)a[0]);
        return done(s, f ? shut(s->files, f) : HANDLE, NULL, false);
    case LIMBA_RT_FILE_FLUSH:
        f = find(s, (int64_t)a[0]);
        return done(s, f ? drain(f) : HANDLE, NULL, false);
    case LIMBA_RT_FILE_READ_LINE: {
        char *line;
        size_t len;
        bool got;
        meri_str *x;
        int res;
        if (meri_mem_wrong(a[1], sizeof(uint64_t)))
            return wrong(s);
        if (!(f = find(s, (int64_t)a[0])))
            return done(s, HANDLE, NULL, false);
        if ((res = read_line(f, &line, &len, &got)) != OK)
            return done(s, res, NULL, false);
        x = meri_state_str(s, line, len);
        free(line);
        if (!x)
            return meri_rt_trap(s, LIMBA_TRAP_NOMEM);
        store_str(s, a[1], meri_str_value(x));
        a[0] = got;
        return 1;
    }
    case LIMBA_RT_FILE_READ:
    case LIMBA_RT_FILE_WRITE: {
        int64_t n = (int64_t)a[2];
        uint8_t *p = (uint8_t *)(uintptr_t)MERI_ADDR(a[1]);
        size_t got = 0;
        int res;
        if (n < 0 || meri_mem_wrong(a[1], (uint64_t)n))
            return wrong(s);
        if (!(f = find(s, (int64_t)a[0])))
            return done(s, HANDLE, NULL, false);
        if (id == LIMBA_RT_FILE_WRITE)
            return done(s, write_file(f, p, (size_t)n), NULL, false);
        res = read_file(f, p, (size_t)n, &got);
        a[0] = got;
        return done(s, res, NULL, false);
    }
    }
    return -1;
}

bool meri_files_end(meri_state *s)
{
    struct meri_files *t = s->files;
    bool ok = true;
    int i;

    if (!t)
        return true;
    for (i = 0; i < MERI_FILES_MAX; i++)
        if (t->f[i].handle && shut(t, &t->f[i]) != OK)
            ok = false;
    free(t);
    s->files = NULL;
    return ok;
}
