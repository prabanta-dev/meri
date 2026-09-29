#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
# Build script for Meri: ./build.sh --help says it all.
# No -march=native: nothing here depends on the CPU it is built on.
set -euo pipefail

cd "$(dirname "$0")"

usage()
{
    cat <<'EOF'
Meri build script.

  ./build.sh [variant] [action]

  variant: release (default) | debug | asan | ubsan | tsan
  action:  build (default) | test | clean | clean-build | clean-test

Meri reads the IR of Limba and links its library, liblimba.a: build and
test first build Limba, in the same variant, with its own build.sh.

Variants
  release  -O2, assertions off. What is shipped, and the only build a
           speed number may come from.
  debug    -O0 -g. Slow but honest under a debugger.
  asan     AddressSanitizer: out-of-bounds reads and writes, use after
           free, leaks at exit. About 2x slower.
  ubsan    UndefinedBehaviorSanitizer: signed overflow, oversized shifts,
           misaligned or null pointers. Stops at the first one.
  tsan     ThreadSanitizer: data races.

  A sanitizer reports at run time, so it has to be the `test` action, or
  a program of the variant run by hand. Their programs carry a -<variant>
  suffix (bin/<cpu>-<os>/test_vm-asan), so they never overwrite the
  release build, and each variant keeps its own objects.

Actions
  build        build Limba, then compile the library, the tests and the
               programs
  test         build, then run every test_* of the variant from the root
               of the repository (they read tests/, write nothing); exit 1
               if one fails
  clean        remove the objects of the variant and the programs it made,
               and nothing else (Limba is left alone)
  clean-build  the two in a row, for a build from nothing
  clean-test   the same, then the tests: the honest answer before a commit

Output
  Objects and the static library go to lib/<cpu>-<os>-<variant>/,
  programs to bin/<cpu>-<os>/, with a -<variant> suffix for every variant
  but release. Each src/<tool>/main.c becomes the program <tool> (meri,
  prabanta), with the other .c files of its directory; every other .c
  under src/ goes into libmeri.a. Each tests/*.c and tools/*.c becomes a
  program too. Every program is linked with libmeri.a and liblimba.a, and
  knows where Limba is (MERI_LIMBA_DIR): test_vm runs the cases of its
  reference interpreter too, LIMBA_DIR/tests/eval.

Environment
  CC            the compiler, gcc by default
  AR            the archiver, ar by default
  LIMBA_DIR     where Limba is, ../Limba by default
  MERI_CFLAGS   flags added to the variant, to try a compiler out (Limba
                takes its own, LIMBA_CFLAGS)

Before a commit: ./build.sh release test and at least asan.

Examples
  ./build.sh                 a release build
  ./build.sh release test    build it and run the tests
  ./build.sh asan test       the same under AddressSanitizer
  ./build.sh asan clean      throw the asan objects away
EOF
}

case "${1:-}" in
-h | --help | help)
    usage
    exit 0
    ;;
esac

VARIANT="${1:-release}"
ACTION="${2:-build}"
CC="${CC:-gcc}"

CPU="$(uname -m)"
OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
OBJ="lib/$CPU-$OS-$VARIANT"
BIN="bin/$CPU-$OS"
SUFFIX=""
[ "$VARIANT" != release ] && SUFFIX="-$VARIANT"

# -ffp-contract=off: a * b + c is two roundings, as in the IR, never an
# FMA the compiler picks where the CPU has one (the same bits as lir_run)
CFLAGS=(-std=gnu11 -Wall -Wextra -Werror -pthread -ffp-contract=off -Isrc
    -Iinclude)
case "$VARIANT" in
release) CFLAGS+=(-O2 -DNDEBUG) ;;
debug) CFLAGS+=(-O0 -g) ;;
asan) CFLAGS+=(-O1 -g -fsanitize=address -fno-omit-frame-pointer) ;;
ubsan) CFLAGS+=(-O1 -g -fsanitize=undefined -fno-sanitize-recover=all) ;;
tsan) CFLAGS+=(-O1 -g -fsanitize=thread) ;;
*)
    echo "build.sh: unknown variant: $VARIANT" >&2
    case "$VARIANT" in
    build | test | clean)
        echo "build.sh: the variant comes first: ./build.sh release" \
            "$VARIANT" >&2 ;;
    esac
    echo "build.sh: --help says which variants there are" >&2
    exit 2
    ;;
esac

case "$ACTION" in
build | test | clean | clean-build | clean-test) ;;
*)
    echo "build.sh: unknown action: $ACTION" >&2
    echo "build.sh: --help says which actions there are" >&2
    exit 2
    ;;
esac
LDLIBS=(-lm)
if [ -n "${MERI_CFLAGS:-}" ]; then
    # shellcheck disable=SC2206
    CFLAGS+=($MERI_CFLAGS)
    LDLIBS+=($MERI_CFLAGS)
fi
AR="${AR:-ar}"
VERSION="$(git describe --always --dirty 2>/dev/null || echo unknown)"
CFLAGS+=(-DMERI_VERSION="\"$VERSION\"")

# Throw away what this variant made, and only what this variant made.
clean()
{
    rm -rf "$OBJ"
    if [ -n "$SUFFIX" ]; then
        rm -f "$BIN"/*"$SUFFIX"
        return
    fi
    for f in "$BIN"/*; do
        [ -e "$f" ] || continue
        case "$f" in
        *-debug | *-asan | *-ubsan | *-tsan) continue ;;
        esac
        rm -f "$f"
    done
}

case "$ACTION" in
clean)
    clean
    exit 0
    ;;
clean-build)
    clean
    ACTION=build
    ;;
clean-test)
    clean
    ACTION=test
    ;;
esac

# Limba first, in the same variant: its public headers and its library
LIMBA_DIR="${LIMBA_DIR:-../Limba}"
if [ ! -x "$LIMBA_DIR/build.sh" ]; then
    echo "build.sh: Limba not found in $LIMBA_DIR (set LIMBA_DIR)" >&2
    exit 2
fi
LIMBA_DIR="$(cd "$LIMBA_DIR" && pwd)"
echo "LIMBA $LIMBA_DIR ($(git -C "$LIMBA_DIR" describe --always --dirty \
    2>/dev/null || echo unknown))"
"$LIMBA_DIR/build.sh" "$VARIANT" build >/dev/null
LIBLIMBA="$LIMBA_DIR/lib/$CPU-$OS-$VARIANT/liblimba.a"
CFLAGS+=(-I"$LIMBA_DIR/include" -DMERI_LIMBA_DIR="\"$LIMBA_DIR\"")

mkdir -p "$OBJ" "$BIN"

# one object, rebuilt when its source or any header or table is newer,
# Limba's public ones too
srcdirs=()
for d in src include "$LIMBA_DIR/include"; do
    [ -d "$d" ] && srcdirs+=("$d")
done
headers_newest=$(find "${srcdirs[@]}" \( -name '*.h' -o -name '*.def' \) \
    -printf '%T@\n' | sort -n | tail -1)
headers_newest="${headers_newest:-0}"
compile()
{ # source, object, then the flags
    local src="$1" obj="$2"
    shift 2
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ] ||
        [ "$(stat -c %Y "$obj")" -lt "${headers_newest%.*}" ]; then
        echo "CC  $src"
        "$CC" "$@" -c "$src" -o "$obj"
    fi
}
objname() { echo "$OBJ/$(echo "${1#src/}" | tr '/' '_' | sed 's/\.c$/.o/')"; }

# A directory with a main.c is a program: its other .c files are its own.
# Every other .c under src/ goes into the library.
tool_dirs=$(for m in src/*/main.c; do [ ! -f "$m" ] || dirname "$m"; done)
objs=()
if [ -d src ]; then
    while IFS= read -r src; do
        grep -qx "$(dirname "$src")" <<<"$tool_dirs" && continue
        obj=$(objname "$src")
        objs+=("$obj")
        compile "$src" "$obj" "${CFLAGS[@]}"
    done < <(find src -name '*.c' ! -name main.c ! -path 'src/third_party/*' |
        sort)
    # the code of others, as it came (PROVENANCE in each directory): the
    # flags of Meri but two warnings it does not follow
    while IFS= read -r src; do
        obj=$(objname "$src")
        objs+=("$obj")
        compile "$src" "$obj" "${CFLAGS[@]}" -Wno-sign-compare \
            -Wno-unused-parameter
    done < <(find src/third_party -name '*.c' 2>/dev/null | sort)
fi
rm -f "$OBJ/libmeri.a"
libs=()
if [ ${#objs[@]} -gt 0 ]; then
    "$AR" rcs "$OBJ/libmeri.a" "${objs[@]}"
    libs+=("$OBJ/libmeri.a")
fi
libs+=("$LIBLIMBA")

# tests and tools, then the programs
for src in tests/*.c tools/*.c src/*/main.c; do
    [ -f "$src" ] || continue
    exe="$BIN/$(basename "${src%.c}")$SUFFIX"
    own=()
    if [ "$(basename "$src")" = main.c ]; then
        dir=$(dirname "$src")
        exe="$BIN/$(basename "$dir")$SUFFIX"
        while IFS= read -r c; do
            obj=$(objname "$c")
            own+=("$obj")
            compile "$c" "$obj" "${CFLAGS[@]}"
        done < <(find "$dir" -maxdepth 1 -name '*.c' ! -name main.c | sort)
    fi
    echo "LD  $exe"
    "$CC" "${CFLAGS[@]}" "$src" "${own[@]}" "${libs[@]}" \
        "${LDLIBS[@]}" -o "$exe"
done

if [ "$ACTION" = test ]; then
    status=0
    for t in "$BIN"/test_*"$SUFFIX"; do
        [ -e "$t" ] || continue
        # in release, skip the sanitizer builds (test_x-asan and the like)
        [ -n "$SUFFIX" ] || [[ "$(basename "$t")" != *-* ]] || continue
        echo "RUN $t"
        "$t" || status=1
    done
    # the calls of C (progetto_ir.md § 11e): Limba's probes against its
    # library of known signatures, built here, the output computed by hand
    # by Limba (lir_run does not call C)
    ffi="$LIMBA_DIR/tests/luxia/ffi"
    if [ -f "$ffi/probe.c" ] && [ -x "$BIN/meri$SUFFIX" ]; then
        # the library built by gcc and by clang at -O2: the callees of
        # clang rely on the caller's extension of the narrow integers
        for cc in gcc clang; do
            command -v "$cc" >/dev/null || continue
            echo "RUN probes.luxia against libprobe built by $cc"
            mkdir -p "$OBJ/probe-$cc"
            "$cc" -O2 -shared -fPIC -o "$OBJ/probe-$cc/libprobe.so" \
                "$ffi/probe.c" || status=1
            for o in 0 1; do
                if ! "$LIMBA_DIR/bin/$CPU-$OS/limba$SUFFIX" -O$o \
                    -o "$OBJ/probes-O$o.lir" "$ffi/probes.luxia" ||
                    ! "$BIN/meri$SUFFIX" --lib-path="$OBJ/probe-$cc" \
                        "$OBJ/probes-O$o.lir" >"$OBJ/probes-O$o.out" ||
                    ! cmp -s "$ffi/expected/probes.out" \
                        "$OBJ/probes-O$o.out"; then
                    echo "probes.luxia -O$o, libprobe by $cc:" \
                        "not the expected output"
                    status=1
                fi
            done
        done
    fi
    exit $status
fi
