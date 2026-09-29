/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * ffi.c - externs and call.ext (ffi.h). The signature of each extern is
 * turned once into the types of abi.h: the integers of the IR are
 * canonical, sign-extended from their width; one of 8, 16 or 32 bits
 * passes extended as the marker of the signature says (zext: with zeros,
 * sext or none: with its sign), and a result comes back canonical from
 * its low bits whatever its marker; i1 is the _Bool of C; a struct keeps the
 * layout of its type in the module, an array field as that many fields.
 */
#include "vm/ffi.h"

#include "vm/abi.h"
#include "vm/heap.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct meri_ext {
    void *fn;
    meri_abi_type *ret;  /* the result; void for a struct result */
    meri_abi_type *sret; /* a struct result, written at a[0]; or NULL */
    uint32_t n;          /* the C arguments */
    meri_abi_type **args;
    limba_id *irtype; /* of each C argument, and of the result last */
} meri_ext;

const char *meri_ffi_target(void)
{
    const char *why;

    return meri_abi_available(&why) ? "x86_64-linux" : NULL;
}

/* ---- the types */

static const meri_abi_type *scalar(limba_id t)
{
    switch (t) {
    case LIMBA_T_I1: /* _Bool: one byte, 0 or 1 */
        return &meri_abi_u8;
    case LIMBA_T_I8:
        return &meri_abi_s8;
    case LIMBA_T_I16:
        return &meri_abi_s16;
    case LIMBA_T_I32:
        return &meri_abi_s32;
    case LIMBA_T_I64:
        return &meri_abi_s64;
    case LIMBA_T_F32:
        return &meri_abi_f32;
    case LIMBA_T_F64:
        return &meri_abi_f64;
    case LIMBA_T_PTR:
        return &meri_abi_ptr;
    default:
        return NULL;
    }
}

static void type_free(meri_abi_type *t)
{
    uint32_t i;

    if (!t || t->kind != MERI_ABI_STRUCT)
        return;
    for (i = 0; i < t->nfields; i++)
        type_free((meri_abi_type *)t->fields[i]);
    free((void *)t->fields);
    free((void *)t->offsets);
    free(t);
}

/* the ABI type of type t of m (NULL: no such type in a C signature, or
   memory exhausted); a scalar is shared, a struct made anew */
static meri_abi_type *abi_type(const limba_module *m, limba_id t)
{
    const limba_type *ty = &m->types[t];
    meri_abi_type *s;
    const meri_abi_type **f;
    uint32_t *o, n, i;

    if (ty->kind != LIMBA_TK_STRUCT && ty->kind != LIMBA_TK_ARRAY)
        return (meri_abi_type *)scalar(t);
    n = ty->count;
    s = calloc(1, sizeof(*s));
    f = calloc(n ? n : 1, sizeof(*f));
    o = calloc(n ? n : 1, sizeof(*o));
    if (!s || !f || !o) {
        free(s), free(f), free(o);
        return NULL;
    }
    *s = (meri_abi_type){MERI_ABI_STRUCT, ty->size, ty->align, n, f, o};
    for (i = 0; i < n; i++) {
        limba_id ft = ty->kind == LIMBA_TK_ARRAY
                          ? ty->elem
                          : m->members[ty->first + i].type;
        o[i] = ty->kind == LIMBA_TK_ARRAY ? i * m->types[ty->elem].size
                                          : m->members[ty->first + i].offset;
        if (!(f[i] = abi_type(m, ft))) {
            s->nfields = i;
            type_free(s);
            return NULL;
        }
    }
    return s;
}

static bool is_struct(const limba_module *m, limba_id t)
{
    return m->types[t].kind == LIMBA_TK_STRUCT ||
           m->types[t].kind == LIMBA_TK_ARRAY;
}

/* the type of a parameter or result of a C signature: an integer of 8,
   16 or 32 bits extended as its marker says (zext: unsigned), a struct
   laid out as in the module */
static meri_abi_type *param_type(const limba_module *m, limba_id t,
                                 unsigned ext)
{
    if (ext == LIMBA_EXT_ZEXT)
        switch (t) {
        case LIMBA_T_I8:
            return (meri_abi_type *)&meri_abi_u8;
        case LIMBA_T_I16:
            return (meri_abi_type *)&meri_abi_u16;
        case LIMBA_T_I32:
            return (meri_abi_type *)&meri_abi_u32;
        }
    return abi_type(m, t);
}

/* ---- the libraries */

static void *open_lib(const char *name, const char *const *dirs, size_t ndirs)
{
    char path[4096];
    size_t i;
    bool file = strchr(name, '/') || strstr(name, ".so");
    void *h;

    for (i = 0; i < ndirs; i++) {
        int k =
            file ? snprintf(path, sizeof(path), "%s/%s", dirs[i], name)
                 : snprintf(path, sizeof(path), "%s/lib%s.so", dirs[i], name);
        if (k > 0 && (size_t)k < sizeof(path) &&
            (h = dlopen(path, RTLD_NOW | RTLD_LOCAL)))
            return h;
    }
    if (file)
        return dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (snprintf(path, sizeof(path), "lib%s.so", name) >= (int)sizeof(path))
        return NULL;
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}

static bool fail(meri_diag *d, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(d->msg, sizeof(d->msg), fmt, ap);
    va_end(ap);
    return false;
}

bool meri_ffi_link(meri_program *p, const char *const *dirs, size_t ndirs,
                   meri_diag *d)
{
    const limba_module *m = p->m;
    const char *mine = meri_ffi_target(), *why = "";
    uint32_t x, k;
    size_t n;

    d->msg[0] = 0;
    if (m->target != LIMBA_NONE) {
        const char *t = limba_str(m, m->target, &n);
        if (!mine || strlen(mine) != n || memcmp(mine, t, n))
            return fail(d,
                        "a module for %.*s, and this engine calls the C of %s",
                        (int)n, t, mine ? mine : "no platform");
    }
    if (!m->nexterns)
        return true;
    if (!meri_abi_available(&why))
        return fail(d, "%s", why);
    p->exts = calloc(m->nexterns, sizeof(meri_ext));
    p->libs = calloc(m->nexterns, sizeof(void *));
    p->libnames = calloc(m->nexterns, sizeof(limba_id));
    if (!p->exts || !p->libs || !p->libnames)
        return fail(d, "out of memory");
    p->nexts = m->nexterns;
    for (x = 0; x < m->nexterns; x++) {
        const limba_extern *e = &m->externs[x];
        const limba_type *ft = &m->types[e->type];
        limba_id rt = ft->elem;
        meri_ext *ex = &p->exts[x];
        const char *sym = limba_str(m, e->symbol, &n), *lib = "c";
        size_t nlib = 1, nsym = n;
        char symz[512];
        void *h = RTLD_DEFAULT;
        uint32_t np = ft->count;

        if (e->library != LIMBA_NONE)
            lib = limba_str(m, e->library, &nlib);
        /* the library, opened once */
        if (!(nlib == 1 && lib[0] == 'c')) {
            for (k = 0; k < p->nlibs && p->libnames[k] != e->library; k++)
                ;
            if (k == p->nlibs) {
                char libz[512];
                if (nlib >= sizeof(libz))
                    return fail(d, "library %.*s: a name too long", (int)nlib,
                                lib);
                memcpy(libz, lib, nlib);
                libz[nlib] = 0;
                if (!(p->libs[k] = open_lib(libz, dirs, ndirs)))
                    return fail(d,
                                "library %.*s not found (its directory: "
                                "--lib-path)",
                                (int)nlib, lib);
                p->libnames[k] = e->library;
                p->nlibs++;
            }
            h = p->libs[k];
        }
        if (nsym >= sizeof(symz))
            return fail(d, "symbol %.*s: a name too long", (int)nsym, sym);
        memcpy(symz, sym, nsym);
        symz[nsym] = 0;
        dlerror();
        ex->fn = dlsym(h, symz);
        if (!ex->fn)
            return fail(d, "symbol %.*s not found in library %.*s", (int)nsym,
                        sym, (int)nlib, lib);
        /* the signature */
        ex->irtype = calloc(np + 1, sizeof(limba_id));
        ex->args = calloc(np + 1, sizeof(meri_abi_type *));
        if (!ex->irtype || !ex->args)
            return fail(d, "out of memory");
        ex->n = np;
        ex->irtype[np] = rt;
        ex->ret = (meri_abi_type *)&meri_abi_void;
        k = 0;
        if (rt != LIMBA_T_VOID && is_struct(m, rt)) {
            if (!(ex->sret = abi_type(m, rt)))
                return fail(d, "extern %.*s: a result C cannot take", (int)nsym,
                            sym);
        } else if (rt != LIMBA_T_VOID &&
                   !(ex->ret = param_type(m, rt, ft->rext))) {
            return fail(d, "extern %.*s: a result C cannot take", (int)nsym,
                        sym);
        }
        for (; k < np; k++) {
            /* a parameter's offset is its limba_ext */
            limba_id at = m->members[ft->first + k].type;
            ex->irtype[k] = at;
            if (!(ex->args[k] =
                      param_type(m, at, m->members[ft->first + k].offset)))
                return fail(d, "extern %.*s: an argument C cannot take",
                            (int)nsym, sym);
        }
    }
    return true;
}

/* the canonical value of type t from the bytes of a C result at r */
static uint64_t canonical(limba_id t, const unsigned char *r)
{
    uint64_t v = 0;

    switch (t) {
    case LIMBA_T_I1:
        return r[0] != 0;
    case LIMBA_T_I8:
        return (uint64_t)(int64_t)(int8_t)r[0];
    case LIMBA_T_I16: {
        int16_t x;
        memcpy(&x, r, 2);
        return (uint64_t)(int64_t)x;
    }
    case LIMBA_T_I32: {
        int32_t x;
        memcpy(&x, r, 4);
        return (uint64_t)(int64_t)x;
    }
    case LIMBA_T_F32: {
        uint32_t x;
        memcpy(&x, r, 4);
        return x;
    }
    default:
        memcpy(&v, r, 8);
        return v;
    }
}

bool meri_ffi_call(const meri_program *p, uint32_t x, uint64_t *a,
                   meri_state *s)
{
    const meri_ext *ex;
    const void *vals[MERI_ABI_MAX_ARGS];
    uint64_t words[MERI_ABI_MAX_ARGS];
    unsigned char ret[64];
    const char *why = "";
    void *out;
    uint32_t k, first;

    if (!p->exts || x >= p->nexts) { /* not linked: nothing resolved */
        s->status = MERI_UNSUPPORTED;
        return false;
    }
    ex = &p->exts[x];
    if (ex->n > MERI_ABI_MAX_ARGS) {
        s->status = MERI_UNSUPPORTED;
        return false;
    }
    first = ex->sret != NULL;
    out = first ? (void *)(uintptr_t)MERI_ADDR(a[0]) : ret;
    for (k = 0; k < ex->n; k++) {
        uint64_t v = a[first + k];
        if (ex->args[k]->kind == MERI_ABI_STRUCT) { /* a ptr to its bytes */
            vals[k] = (const void *)(uintptr_t)MERI_ADDR(v);
            continue;
        }
        /* a pointer without its generation, a scalar in its low bytes */
        words[k] = ex->args[k]->kind == MERI_ABI_PTR ? MERI_ADDR(v) : v;
        vals[k] = &words[k];
    }
    if (!meri_abi_call(ex->fn, first ? ex->sret : ex->ret, ex->n,
                       (const meri_abi_type *const *)ex->args, vals, out,
                       &why)) {
        s->status = MERI_UNSUPPORTED;
        return false;
    }
    if (!first && ex->ret->kind != MERI_ABI_VOID)
        a[0] = canonical(ex->irtype[ex->n], ret);
    return true;
}

void meri_ffi_free(meri_program *p)
{
    uint32_t x, k;

    for (x = 0; p->exts && x < p->nexts; x++) {
        meri_ext *ex = &p->exts[x];
        for (k = 0; ex->args && k < ex->n; k++)
            type_free(ex->args[k]);
        type_free(ex->ret);
        type_free(ex->sret);
        free(ex->args);
        free(ex->irtype);
    }
    for (k = 0; p->libs && k < p->nlibs; k++)
        dlclose(p->libs[k]);
    free(p->exts);
    free(p->libs);
    free(p->libnames);
    p->exts = NULL;
    p->libs = NULL;
    p->libnames = NULL;
    p->nexts = p->nlibs = 0;
}
