#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
#
# The proof that a change leaves the bytecode as it was: the fixed step of
# every change to Meri that must not touch it.
#
#   tools/identity.sh [-w WORK] [-l LIMBA] [-x LUXIA] DIR
#                                hashes of every module into DIR/hash
#   tools/identity.sh -c A B     the modules whose hashes differ
#
# WORK is where the modules are made (tmp/meri/ident in the home directory
# of the user by default), LIMBA and LUXIA where those projects are
# (../Limba and ../Luxia next to Meri by default). Nothing comes from the
# environment: another session could export a variable.
#
# Before the change, ./build.sh release and tools/identity.sh before; after
# it, the same into after; then -c before after must list nothing. A
# sabotage of the change (one that does touch the bytecode) must list
# something: else the proof does not see the change.
#
# The modules: the programs of Luxia's lx_gen and lx_gen -d, seeds 1..500,
# made into IR by luxia --emit=lir at -O0 and -O1; the benchmarks of
# Luxia (with those that call C, in benchmarks/c), at -O0 and -O1; the .lit of tests/vm and of Limba's tests/eval,
# through limba. Their .lir are made once, in
# WORK/lir, and reused: before and
# after compare the same IR. For each one, the hash of the bytecode as
# bytes (bytecode_dump) and of meri --disasm. Every program runs under
# tools/sorvegliato.sh.
set -u

MERI_DIR=$(cd "$(dirname "$0")/.." && pwd)
LIMBA_DIR="$MERI_DIR/../Limba"
LUXIA_DIR="$MERI_DIR/../Luxia"
WORK="$(getent passwd "$(id -u)" | cut -d: -f6)/tmp/meri/ident"
if [ "${1:-}" != -c ]; then
    while [ $# -gt 0 ]; do
        case "$1" in
        -w) WORK="$2"; shift 2 ;;
        -l) LIMBA_DIR="$2"; shift 2 ;;
        -x) LUXIA_DIR="$2"; shift 2 ;;
        -*) echo "identity: unknown option: $1" >&2; exit 2 ;;
        *) break ;;
        esac
    done
fi
LB="$LIMBA_DIR/bin/x86_64-linux"
XB="$LUXIA_DIR/bin/x86_64-linux"
MB="$MERI_DIR/bin/x86_64-linux"
SORV="$MERI_DIR/tools/sorvegliato.sh"
LIR="$WORK/lir"

if [ "${1:-}" = -c ]; then
    [ $# -eq 3 ] || { echo "usage: identity.sh -c A B" >&2; exit 2; }
    if cmp -s "$2/hash" "$3/hash"; then
        echo "identity: $(wc -l <"$2/hash") modules, all the same"
        exit 0
    fi
    # a module that differs, or that one side lacks, is a difference
    awk 'FNR == NR { a[$1] = $2 " " $3; next }
         { if (!($1 in a)) { print $1 ": only in B"; o++ }
           else if (a[$1] != $2 " " $3) { print $1; d++ }
           delete a[$1] }
         END { for (n in a) { print n ": only in A"; o++ }
               printf "identity: %d modules differ, %d on one side only\n",
                   d, o > "/dev/stderr" }' "$2/hash" "$3/hash"
    exit 1
fi
[ $# -eq 1 ] || {
    echo "usage: identity.sh [-w WORK] [-l LIMBA] [-x LUXIA] DIR | -c A B" >&2
    exit 2
}
OUT="$1"
for x in "$XB/lx_gen" "$XB/luxia" "$LB/limba" "$MB/meri" \
    "$MB/bytecode_dump"; do
    [ -x "$x" ] || { echo "identity: $x missing: ./build.sh release" \
        "(here and in Luxia)" >&2
        exit 2; }
done
mkdir -p "$OUT" "$LIR" "$WORK/d" || exit 1

if [ ! -e "$LIR/done" ]; then
    for s in $(seq 1 500); do
        "$XB/lx_gen" "$s" >"$LIR/lx$s.luxia"
        "$XB/lx_gen" -d "$WORK/d/lxd$s" "$s"
        std=()
        [ -d "$WORK/d/lxd$s/std" ] && std=(--stdlib="$WORK/d/lxd$s/std")
        for o in O0 O1; do
            "$XB/luxia" --emit=lir -$o -o "$LIR/lx$s-$o.lir" \
                "$LIR/lx$s.luxia" 2>/dev/null
            "$XB/luxia" --emit=lir -$o "${std[@]}" -o "$LIR/lxd$s-$o.lir" \
                "$WORK/d/lxd$s/prog/prog.luxia" 2>/dev/null
        done
    done
    for b in "$LUXIA_DIR"/tests/benchmarks/*.luxia \
        "$LUXIA_DIR"/tests/benchmarks/c/*.luxia; do
        [ -e "$b" ] || continue
        n=$(basename "$b" .luxia)
        for o in O0 O1; do
            "$XB/luxia" --emit=lir -$o -o "$LIR/b-$n-$o.lir" "$b" \
                2>/dev/null
        done
    done
    for t in "$MERI_DIR"/tests/vm/*.lit "$LIMBA_DIR"/tests/eval/*.lit; do
        n=$(basename "$(dirname "$t")")-$(basename "$t" .lit)
        "$LB/limba" -o "$LIR/t-$n.lir" "$t" 2>/dev/null
    done
    touch "$LIR/done"
fi
for f in "$LIR"/*.lir; do
    n=$(basename "$f" .lir)
    a=$("$SORV" -t 60 -f 256 -- "$MB/bytecode_dump" "$f" | sha256sum |
        cut -c1-16)
    b=$("$SORV" -t 60 -f 256 -- "$MB/meri" --disasm "$f" 2>&1 | sha256sum |
        cut -c1-16)
    echo "$n $a $b"
done >"$OUT/hash"
echo "identity: $(wc -l <"$OUT/hash") modules hashed into $OUT/hash"
