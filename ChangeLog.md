# Changelog

## Unreleased

- `meri` reads a module of the IR (`.lir`), verifies it with the verifier
  of Limba and prints a summary: tables, functions, operations.
- A first virtual machine: the IR (version 3) compiled to bytecode of
  fixed 4-byte words, a register of 64 bits a value, windows of registers
  a call; `meri` runs a module, `--disasm` prints its bytecode. Checks,
  traps and positions as the reference interpreter of Limba; a deep
  recursion is the trap STACK, never a crash. First cut: strings and
  freed blocks are not reused, at most 256 registers a function.
- `test_vm` runs the cases of `tests/vm` and those of the reference
  interpreter; `fuzz_meri` is the fuzzing target of the compiler.
- The repository: build script (it builds Limba first, in the same
  variant, and links every program with its library), licence, style.
