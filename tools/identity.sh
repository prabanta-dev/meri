#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
#
# The proof that a change leaves the bytecode as it was: the fixed step of
# every change to Meri that must not touch it.
#
#   tools/identity.sh DIR        hashes of every module into DIR/hash
#   tools/identity.sh -c A B     the modules whose hashes differ
#
# Before the change, ./build.sh release and tools/identity.sh before; after
# it, the same into after; then -c before after must list nothing. A
# sabotage of the change (one that does touch the bytecode) must list
# something: else the proof does not see the change.
#
# The modules: the programs of lx_gen and lx_gen -d, seeds 1..500, at -O0
# and -O1; the benchmarks of Limba and of Meri, at -O0 and -O1; the .lit
# of tests/vm and of Limba's tests/eval. Their .lir are made once, in
# $IDENT_DIR/lir (~/tmp/meri/ident/lir by default), and reused: before and
# after compare the same IR. For each one, the hash of the bytecode as
# bytes (bytecode_dump) and of meri --disasm. Every program runs under
# tools/sorvegliato.sh.
set -u

MERI_DIR=$(cd "$(dirname "$0")/.." && pwd)
LIMBA_DIR="${LIMBA_DIR:-$MERI_DIR/../Limba}"
LB="$LIMBA_DIR/bin/x86_64-linux"
MB="$MERI_DIR/bin/x86_64-linux"
SORV="$MERI_DIR/tools/sorvegliato.sh"
WORK="${IDENT_DIR:-$HOME/tmp/meri/ident}"
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
[ $# -eq 1 ] || { echo "usage: identity.sh DIR | -c A B" >&2; exit 2; }
OUT="$1"
for x in "$LB/lx_gen" "$LB/limba" "$MB/meri" "$MB/bytecode_dump"; do
    [ -x "$x" ] || { echo "identity: $x missing: ./build.sh release" >&2
        exit 2; }
done
mkdir -p "$OUT" "$LIR" "$WORK/d" || exit 1

if [ ! -e "$LIR/done" ]; then
    for s in $(seq 1 500); do
        "$LB/lx_gen" "$s" >"$LIR/lx$s.luxia"
        "$LB/lx_gen" -d "$WORK/d/lxd$s" "$s"
        std=()
        [ -d "$WORK/d/lxd$s/std" ] && std=(--stdlib="$WORK/d/lxd$s/std")
        for o in O0 O1; do
            "$LB/limba" -$o -o "$LIR/lx$s-$o.lir" "$LIR/lx$s.luxia" \
                2>/dev/null
            "$LB/limba" -$o "${std[@]}" -o "$LIR/lxd$s-$o.lir" \
                "$WORK/d/lxd$s/prog/prog.luxia" 2>/dev/null
        done
    done
    for b in "$LIMBA_DIR"/tests/luxia/benchmarks/*.luxia \
        "$MERI_DIR"/tests/luxia/benchmarks/*.luxia; do
        [ -e "$b" ] || continue
        n=$(basename "$b" .luxia)
        for o in O0 O1; do
            "$LB/limba" -$o -o "$LIR/b-$n-$o.lir" "$b" 2>/dev/null
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
