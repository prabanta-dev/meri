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
- Faster, same results: the allocator lets a result take the register of
  an operand that dies in the same instruction, and gives a parameter of
  a block the register of its arguments, so their copies vanish; small
  blocks of `mem_alloc` come from arenas of one size class, with the same
  generations and exact checks; `math_sqrt` and the other functions of
  one real become instructions (`FSQRT`, `FMATH`), and `ptr_live` with
  its check one instruction (`CHKLIVE`). Building with `-DMERI_PROFILE`
  counts the instructions run, their pairs and the runtime calls.
- More fusions, from the profile: the range check of an index (two
  comparisons, and, check) is one instruction (`CHKR`, or `CHKRK` with
  constant limits); an `addr` used only by the load or store right after
  it goes into it (`LDX*`, `STX*`), with a `sub` of a constant folded
  into the displacement; `str_ptr`, `str_len` and `print_byte` are
  instructions. The allocator keeps the operands of a fused sequence
  alive until the instruction that reads them.
- The constants of a function (integers, reals, null) are loaded once,
  before block 0, instead of on every pass through a loop; liveness sees
  them defined there. If the registers would not fit, the function is
  emitted without hoisting rather than wide.
- Only the constants used on a cycle of the CFG are hoisted (a function
  without loops, called often, loaded them all at every call); a copy
  that changes no bit of a canonical value lives in the register of its
  source; an addr folds into its load or store anywhere later in the
  block; the blocks are laid out so that the way round a loop falls
  through and its exits jump.
- Lighter calls: a call copies its arguments itself (`CALLN`,
  `CALLRTN`: their registers follow it, four to a word) instead of a
  move each; a function without slots costs nothing to the memory budget
  when called; `print_char` is an instruction (`PUTC`).
- The end of a loop, compare, add and jump back, is one instruction
  (`LOOP`, `LOOP32`) when the sum goes where the counter was and the jump
  copies nothing; the blocks are laid out in reverse postorder, the exits
  of a loop after it, and liveness numbers them in that order, so fewer
  copies are left. A check of `ne x, 0` is a check of x; a branch on
  `eq` or `ne` with 0 is a `TEST`; an address at a constant index is the
  displacement of its load or store, `ADDI`, or the register of its base;
  a copy may share the register of any parameter of a block. Instructions
  run: from -13 % (k-nucleotide) to -22 % (binary-trees).
- A pointer given by `str_ptr` keeps its string alive while it is used:
  a string whose last use was `str_ptr` was freed before its bytes were
  read (Limba's `s[i]` on a temporary string, and `str_ptr` taken out of
  a loop). Such a pointer may go only to an `addr`, a load or store
  address, `memcpy`, `memset` or a call; elsewhere the function is
  refused. `load.inv` (IR of Limba d5d3ab8) runs as a load.
- More instructions from the profile: `mem_alloc` (`ALLOC`, and a memset
  of 0 right after it is dropped: blocks are zeroed), `ptr_live`
  (`LIVE`), the nil and dangling checks of an access at one position
  (`CHKNL`), a range check with the subtraction of its lower limit
  (`CHKRS`), a check of an integer comparison (`CHKCC`), memcpy and
  memset of a constant length (`MEMCPYK`, `MEMSETK`). A dangling check
  finds a small block without a division. Instructions run:
  k-nucleotide -12 %, binary-trees -14 %.
- Outside loops, constants of 8 bits go into the instruction: add and
  sub (`ADDK`, `ADDK32`, `ADD.OVK`, `ADD.OVK32`), a comparison fused
  into its branch (`JEQK`, `JLTK`, `JLEK`), `ret` of a constant (`RETK`),
  `mem_alloc` of a constant size (`ALLOCK`); inside loops the constants
  are in registers already. The result of a call goes straight to its
  register (`CALLND`), and `mem_free` is an instruction (`FREE`).
  binary-trees runs 17 % fewer instructions.
- The check of a small block alive is in line in the interpreter, the
  arena found last is remembered (arenas are never freed during a run),
  and the classes whose step is a power of two check it with a mask; the
  budget is counted in line, and new small blocks of 16 and 32 bytes are
  zeroed in line. binary-trees: 23 % fewer cycles.
- The pointer to a large block found alive last is remembered, and
  forgotten when a large block is freed: k-nucleotide's table no longer
  costs two lookups an access (24 % fewer cycles). The mod of Luxia,
  seven instructions of the IR, is one (`SMOD`, `SMOD32`).
- A load right after a store to the same address, of the same type, is
  the value stored, in its register; a call without arguments returns
  straight to its register too (`CALLND`).
- An f64 fmul used once by an fadd, fsub or fmul is one instruction with
  it (`FMADD`, `FMSUB`, `FMUL3` and their reversed forms): two roundings,
  as the IR says, never an FMA; the operands in the order of the IR.
- The repository: build script (it builds Limba first, in the same
  variant, and links every program with its library), licence, style.
