#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
"""Write tests/vm/wide.lit: a function with more values alive at once than
a window has registers (256), so that Meri emits it wide (lower.c). Every
kind of operand meets a high register: arithmetic, a compare and branch, a
switch, select, fma, memory, a string counted, calls of the program and of
the runtime, a loop whose parameters are swapped.

    tools/wide_lit.py > tests/vm/wide.lit
"""
N = 300  # values alive together


def f64(x):
    return float.hex(x)  # the text form writes a real in hexadecimal


out = []  # the expected lines
v = [0]


def new():
    v[0] += 1
    return "v%d" % v[0]


body = []
vals = []
for i in range(N):
    x = new()
    body.append("  %s = iconst i64 %d" % (x, 7 * i + 1))
    vals.append(x)
# a string in a high register, counted
sa, sb, s = new(), new(), new()
body += ['  %s = sconst str "wi"' % sa, '  %s = sconst str "de"' % sb,
         "  %s = call.rt str str_concat(%s, %s)" % (s, sa, sb)]
# a call of the program with two high arguments, a result high
c = new()
body.append("  %s = call i64 @diff(%s, %s)" % (c, vals[-1], vals[-2]))
# and indirectly, the callee in a high register too
fp, ci = new(), new()
body += ["  %s = faddr ptr @diff" % fp,
         "  %s = call.ind i64 fn(i64, i64) -> i64 %s(%s, %s)" % (
             ci, fp, vals[-3], vals[-1])]
# select and fma on high registers
one, cond, sel = new(), new(), new()
body += ["  %s = iconst i64 1" % one,
         "  %s = icmp.eq i1 %s, %s" % (cond, vals[0], one),
         "  %s = select i64 %s, %s, %s" % (sel, cond, vals[-1], vals[-3])]
fa, fb, fm = new(), new(), new()
body += ["  %s = fconst f64 %s" % (fa, f64(1.5)),
         "  %s = fconst f64 %s" % (fb, f64(2.0)),
         "  %s = fma f64 %s, %s, %s" % (fm, fa, fb, fa)]
# memory through a high pointer
p, ld = new(), new()
body += ["  %s = slot ptr $0" % p, "  store %s, %s" % (vals[-5], p),
         "  %s = load i64 %s" % (ld, p)]
head = ["  br b1(%s, %s, %s)" % (vals[-1], vals[-2], one)]
# b1: a loop with swapped parameters, all the values alive through it
pa, pb, pk = new(), new(), new()
loop = ["b1(%s: i64, %s: i64, %s: i64):" % (pa, pb, pk)]
k1, k2, lim, lt = new(), new(), new(), new()
loop += ["  %s = iconst i64 1" % k1,
         "  %s = add i64 %s, %s" % (k2, pk, k1),
         "  %s = iconst i64 4" % lim,
         "  %s = icmp.slt i1 %s, %s" % (lt, k2, lim),
         "  cbr %s, b1(%s, %s, %s), b2" % (lt, pb, pa, k2)]
# b2: the sum of every value, in reverse, printed with the rest
b2 = ["b2:"]
acc = vals[-1]
for x in reversed(vals[:-1]):
    y = new()
    b2.append("  %s = add i64 %s, %s" % (y, acc, x))
    acc = y
sw = new()
b2 += ["  call.rt void print_i64(%s)" % acc, "  call.rt void print_nl()",
       "  call.rt void print_i64(%s)" % c, "  call.rt void print_nl()",
       "  call.rt void print_i64(%s)" % ci, "  call.rt void print_nl()",
       "  call.rt void print_i64(%s)" % sel, "  call.rt void print_nl()",
       "  call.rt void print_f64(%s)" % fm, "  call.rt void print_nl()",
       "  call.rt void print_i64(%s)" % ld, "  call.rt void print_nl()",
       "  call.rt void print_i64(%s)" % pa, "  call.rt void print_nl()",
       "  call.rt void print_str(%s)" % s, "  call.rt void print_nl()",
       "  %s = trunc i32 %s" % (sw, vals[2]),
       "  switch %s, b4 [15: b3]" % sw]
b3 = ["b3:", '  call.rt void print_i64(%s)' % vals[2], "  call.rt void print_nl()",
      "  %s = iconst i64 0" % new(), "  ret v%d" % v[0]]
b4 = ["b4:", "  %s = iconst i64 1" % new(), "  ret v%d" % v[0]]

total = sum(7 * i + 1 for i in range(N))
# the loop: (a, b, k) = (v[-1], v[-2], 1) -> swap while k + 1 < 4: 2 swaps
a, b = 7 * (N - 1) + 1, 7 * (N - 2) + 1
for _ in range(2):
    a, b = b, a
out = [str(total), str(7 * (N - 1) + 1 - (7 * (N - 2) + 1)),
       str(7 * (N - 3) + 1 - (7 * (N - 1) + 1)),
       str(7 * (N - 1) + 1), "4.5", str(7 * (N - 5) + 1), str(a), "wide",
       str(7 * 2 + 1)]

print("\n".join("; output: " + x for x in out))
print("; result: 0")
print("; strings: 0")
print("; %d values alive together: past the 256 registers of a window, the"
      % N)
print("; function is emitted wide (lower.c). Written by tools/wide_lit.py")
print("""func @diff : fn(i64, i64) -> i64 {
b0(v0: i64, v1: i64):
  v2 = sub i64 v0, v1
  ret v2
}
""")
print("func @main : fn() -> i64 {\n  slot 8 align 8\nb0:")
print("\n".join(body + head + loop + b2 + b3 + b4))
print("}")
