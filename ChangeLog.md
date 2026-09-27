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
- IR version 4: a trap is worded by the module (its language as the
  prefix, its texts or those of `traps.def`), `meri:` for Meri's own
  errors; the numbers of `val` are read by Limba's `limba/val.h`; a run
  has a memory budget (`--max-memory`), blocks, strings, globals and slots
  counted against it, "out of memory" past it.
- Strings count their references: in registers (a value owns one, from
  its definition to where it stops living, found by liveness at each
  instruction and edge) and in memory (store, load, retain and release
  of a type, typed slots released at every return); the last reference
  frees the string. `test_vm` checks how many are alive at the end.
- The memory of a freed block is reused. In STRICT a pointer carries the
  generation of its block in its 16 high bits, new at every block at the
  same address: a dangling pointer never equals a new one, and `ptr_live`
  and a second `mem_free` stay exact. An address at its last generation
  is not given out again.
- A function may use up to 65535 registers. Past 256 it is emitted wide:
  an operand in a high register comes to a low temporary with `MOVEW`,
  the result goes back, and calls whose arguments begin past 255 take
  their base from a second word; the other instructions do not change.
  `tools/wide_lit.py` writes the case `tests/vm/wide.lit`.
- The repository: build script (it builds Limba first, in the same
  variant, and links every program with its library), licence, style.
