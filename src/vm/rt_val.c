/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright (C) 2026 Maurizio Cammalleri */
/*
 * rt_val.c - the numbers of val (luxia-0.md § 9.4), for str_to_i64,
 * str_to_u64, str_to_f64 and str_to_f32:
 *
 * - spaces and tabs around the number are ignored, then an optional sign;
 * - the number follows the syntax of a Luxia literal (§ 2.4): decimal
 *   digits, or 0x, 0o, 0b and their digits; _ only between two digits; a
 *   real has digits on both sides of the point and a lowercase e;
 * - a real, or an integer read as a real, is rounded once; inf and nan
 *   (nan without a sign) are accepted.
 *
 * Written from the specification. The literal is copied without its _
 * and handed to strtod or strtof, under the C locale Meri never changes.
 */
#include "vm/rt.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int digit_of(char c, unsigned base)
{
    int v = c >= '0' && c <= '9'   ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                   : -1;
    return v >= 0 && (unsigned)v < base ? v : -1;
}

/* the digits of base from p[*i], a _ allowed only between two digits;
   copied to buf at *k without the _. false if there is no digit */
static bool digits(const char *p, size_t n, size_t *i, unsigned base, char *buf,
                   size_t *k)
{
    size_t start = *i;

    while (*i < n) {
        if (digit_of(p[*i], base) >= 0)
            buf[(*k)++] = p[(*i)++];
        else if (p[*i] == '_' && *i > start && *i + 1 < n &&
                 digit_of(p[*i + 1], base) >= 0)
            (*i)++;
        else
            break;
    }
    return *i > start;
}

enum { NOT_A_LITERAL, INT_LITERAL, REAL_LITERAL };

/* p[0..n) as a literal, copied to buf (n + 1 bytes) without its _ and,
   for a based integer, without its prefix; *base is its base */
static int literal(const char *p, size_t n, char *buf, unsigned *base)
{
    size_t i = 0, k = 0;
    int kind = INT_LITERAL;

    *base = 10;
    if (n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'o' || p[1] == 'b')) {
        *base = p[1] == 'x' ? 16 : p[1] == 'o' ? 8 : 2;
        i = 2;
        if (!digits(p, n, &i, *base, buf, &k))
            return NOT_A_LITERAL;
    } else {
        if (!digits(p, n, &i, 10, buf, &k))
            return NOT_A_LITERAL;
        if (i < n && p[i] == '.') {
            buf[k++] = p[i++];
            if (!digits(p, n, &i, 10, buf, &k))
                return NOT_A_LITERAL;
            kind = REAL_LITERAL;
        }
        if (i < n && p[i] == 'e') {
            buf[k++] = p[i++];
            if (i < n && (p[i] == '+' || p[i] == '-'))
                buf[k++] = p[i++];
            if (!digits(p, n, &i, 10, buf, &k))
                return NOT_A_LITERAL;
            kind = REAL_LITERAL;
        }
    }
    buf[k] = 0;
    return i == n ? kind : NOT_A_LITERAL;
}

/* the number of s without the blanks around and its sign ('+', '-' or 0);
   false if nothing is left */
static bool body(const meri_str *s, const char **p, size_t *n, char *sign)
{
    const char *b = s->data, *e = s->data + s->len;

    while (b < e && (*b == ' ' || *b == '\t'))
        b++;
    while (e > b && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    *sign = b < e && (*b == '+' || *b == '-') ? *b++ : 0;
    *p = b;
    *n = (size_t)(e - b);
    return b < e;
}

bool meri_val_int(const meri_str *s, uint64_t *mag, bool *neg)
{
    const char *p;
    size_t n;
    unsigned base;
    char sign, *buf, *c;
    bool ok;

    if (!body(s, &p, &n, &sign))
        return false;
    *neg = sign == '-';
    buf = malloc(n + 1);
    if (!buf)
        return false;
    ok = literal(p, n, buf, &base) == INT_LITERAL;
    *mag = 0;
    for (c = buf; ok && *c; c++)
        ok = !__builtin_mul_overflow(*mag, base, mag) &&
             !__builtin_add_overflow(*mag, (uint64_t)digit_of(*c, base), mag);
    free(buf);
    return ok;
}

/* the digits of a based integer (base 2, 8 or 16) as the text of a
   hexadecimal float, 0x...p0, so that strtod rounds it once; NULL when
   memory is exhausted */
static char *as_hex(const char *d, unsigned base)
{
    unsigned b = base == 16 ? 4 : base == 8 ? 3 : 1;
    size_t nd = strlen(d), nbits = nd * b, pad = (4 - nbits % 4) % 4;
    size_t nh = (nbits + pad) / 4, h, q;
    char *text = malloc(nh + 5);

    if (!text)
        return NULL;
    memcpy(text, "0x", 2);
    for (h = 0; h < nh; h++) {
        unsigned v = 0;
        for (q = h * 4; q < h * 4 + 4; q++) {
            unsigned one = 0;
            if (q >= pad) {
                size_t at = q - pad; /* the bit, from the first digit */
                one =
                    ((unsigned)digit_of(d[at / b], base) >> (b - 1 - at % b)) &
                    1;
            }
            v = v << 1 | one;
        }
        text[2 + h] = "0123456789abcdef"[v];
    }
    memcpy(text + 2 + nh, "p0", 3);
    return text;
}

bool meri_val_real(const meri_str *s, bool f32, uint64_t *bits)
{
    const char *p;
    size_t n;
    char sign;
    unsigned base;
    double d;

    if (!body(s, &p, &n, &sign))
        return false;
    if (n == 3 && !memcmp(p, "inf", 3)) {
        d = INFINITY;
    } else if (n == 3 && !memcmp(p, "nan", 3)) {
        if (sign)
            return false; /* a NaN has no sign */
        d = NAN;
    } else {
        char *buf = malloc(n + 1), *text;
        int kind;

        if (!buf)
            return false;
        kind = literal(p, n, buf, &base);
        text = kind == INT_LITERAL && base != 10 ? as_hex(buf, base) : buf;
        if (!text) {
            free(buf);
            return false;
        }
        d = 0;
        if (kind != NOT_A_LITERAL)
            d = f32 ? (double)strtof(text, NULL) : strtod(text, NULL);
        if (text != buf)
            free(text);
        free(buf);
        if (kind == NOT_A_LITERAL || isinf(d))
            return false;
    }
    if (sign == '-')
        d = -d;
    if (f32) {
        float f = (float)d;
        uint32_t u;
        memcpy(&u, &f, sizeof(u));
        *bits = u;
    } else {
        memcpy(bits, &d, sizeof(d));
    }
    return true;
}
