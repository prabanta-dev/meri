# Meri

[![Status: experimental](https://img.shields.io/badge/status-experimental-orange.svg)]()
[![Platform: Linux x86-64](https://img.shields.io/badge/platform-Linux%20x86--64-lightgrey.svg)]()
[![Licence: GPL-3.0-or-later](https://img.shields.io/badge/licence-GPL--3.0--or--later-blue.svg)](LICENSE)
[![LinkedIn](https://img.shields.io/badge/LinkedIn-Maurizio%20Cammalleri-0077B5?logo=linkedin)](https://www.linkedin.com/in/maurizio-cammalleri-80a89a11/)
[![Substack](https://img.shields.io/badge/Substack-Maurizio%20Cammalleri-FF6719?logo=substack)](https://cammalleri.substack.com/)

Meri is the back end of the Prabanta family of compilers, written in C with
no dependencies beyond the C library. It reads the intermediate
representation (IR) made by **Limba**, the front end, and runs it: a
register-based virtual machine first, then AOT and JIT compilation.

Meri takes its ideas from SedaiBasic2, an earlier compiler of the author
written in Free Pascal, not its code.

---

## ⚠️ Read this first

**This is a project in development, not a usable program.** `meri` reads
a module of the IR, compiles it to bytecode and runs it on a virtual
machine: no JIT and no AOT yet.

| | |
|---|---|
| **Stage** | Just started. The bytecode, its file format and the command line **change without notice**. |
| **Platform** | Developed and tested on Linux x86-64 only (Debian 13, GCC). |

## Using it

    limba -O1 program.luxia          # writes program.lir
    meri program.lir [arguments]     # runs it
    meri --disasm program.lir        # prints its bytecode
    meri --summary program.lir       # prints what the module holds
    meri --lib-path=DIR program.lir  # where its C libraries are looked for

A run-time error stops the program with a message on the standard error
and exit status 1; `halt(code)` exits with `code`. The language of the
module words its errors, `luxia: index out of range at program.luxia:12:5`;
Meri's own errors, such as a module it refuses, begin with `meri:`. A
program has the memory `--max-memory` gives it (by default half of the
physical memory): past it, "out of memory", never a crash. An error of
input or output is `input/output error`; a program whose output is closed
(`program | head`) ends with status 141 and no message.

What runs today: all of Luxia 0 that Limba compiles, the integers of any
size (BigInt, on mini-gmp) and calls of C libraries declared in the
program (x86-64 Linux only), with Meri's own calling convention, without
libffi.

## The plan

- `meri program.lir` runs the IR written by `limba`.
- `prabanta program.luxia` does both in one executable, in a repository
  of its own: the two libraries are linked statically, and each function
  goes from the front end to the back end in memory, as soon as it is
  complete, with no file in between.
- The virtual machine keeps one file of 64-bit registers, a window of them
  for each call, and fixed 4-byte instructions.
- Every program must print in Meri what it prints in the reference
  interpreter of Limba, on every engine.

## Building

Meri needs [Limba](https://github.com/prabanta-dev/limba) next to it
(`../Limba`, or `LIMBA_DIR`), at the commit named at the end of the last
commit message of Meri ("Limba: ..."): its build script builds Limba
first. A later Limba may change the IR; until Meri follows, it refuses
the modules of that Limba ("written against another runtime or operation
table").

    ./build.sh               # release build
    ./build.sh release test  # and the tests
    ./build.sh --help        # variants (debug, asan, ubsan, tsan), actions

A C11 compiler (GCC or Clang) and a POSIX shell are all it needs.

## Licence

GPL-3.0-or-later, see [`LICENSE`](LICENSE). The authors, and the code of
others compiled into Meri (mini-gmp, under its own licences), are in
[`AUTHORS`](AUTHORS).
