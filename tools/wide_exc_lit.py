#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
"""Write tests/vm/wide_exc.lit: an exception caught in a function emitted
wide (lower.c): more values alive at once than a window has registers, all
of them needed by the handler, so that its parameters are high registers
(CATCH writes them through the low temporaries) and so is a string the pad
releases.

    tools/wide_exc_lit.py > tests/vm/wide_exc.lit
"""
N = 300  # values alive together, through the handler

body, vals = [], []
for i in range(N):
    body.append("  v%d = iconst i64 %d" % (i, 1000 + 7 * i))
    vals.append("v%d" % i)
v = N


def new():
    global v
    v += 1
    return "v%d" % v


total = sum(1000 + 7 * i for i in range(N))
sa, s = new(), new()
lines = ["; output: %d" % total, "; output: wide 1000", "; result: 0",
         "; strings: 0",
         "; an exception caught where 300 values are alive (tools/wide_exc_lit.py):",
         "; the parameters of the handler and a string released by the pad are",
         "; past register 255",
         'exception 1000 "Wide"', "",
         "func @boom : fn() -> void {", "b0:",
         "  v0 = iconst i32 1000", "  v1 = iconst i32 0",
         '  v2 = sconst str "wide"', "  raise v0, v1, v2", "}", "",
         "func @main : fn() -> i64 {", "b0:"] + body + ["  br b1",
         "b1 handler b2:", '  %s = sconst str "s"' % sa,
         "  %s = call.rt str str_concat(%s, %s)" % (s, sa, sa),
         "  call void @boom()", "  call.rt void print_str(%s)" % s,
         "  %s = iconst i64 1" % new(), "  ret v%d" % v]
pi, pp, pm = new(), new(), new()
lines.append("b2(%s: i32, %s: i32, %s: str):" % (pi, pp, pm))
acc = vals[0]
for x in vals[1:]:
    t = new()
    lines.append("  %s = add i64 %s, %s" % (t, acc, x))
    acc = t
lines.append("  call.rt void print_i64(%s)" % acc)
lines.append("  call.rt void print_nl()")
lines.append("  call.rt void print_str(%s)" % pm)
sp = new()
lines.append("  %s = iconst i32 32" % sp)
lines.append("  call.rt void print_char(%s)" % sp)
e = new()
lines.append("  %s = sext i64 %s" % (e, pi))
lines.append("  call.rt void print_i64(%s)" % e)
lines.append("  call.rt void print_nl()")
z = new()
lines.append("  %s = iconst i64 0" % z)
lines.append("  ret %s" % z)
lines.append("}")
print("\n".join(lines))
