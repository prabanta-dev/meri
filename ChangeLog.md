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
- An addr of a constant displacement (1 to 255) used only as the address
  of loads and stores is folded into each of them, with no register; its
  base is kept alive wherever the addr is used.
- The end of a loop going down by a sub is one instruction too (`LOOPD`,
  `LOOPD32`); a load of i8 used only by the zext right after it is one
  zero-extended load (`LDU8`, `LDXU8`). The strings that die at an
  instruction fused into a later one are released after that one.
- Outside loops, sdiv by a constant of 2 to 127 carries it (`SDIVK`), a
  power of two by a shift that truncates toward zero as sdiv does.
- A range check with limits in registers and the addr of its index right
  after it are one instruction (`CHKADDR`, three words); `CHKRS` also
  with a constant lower limit, when its sub is not folded into an addr.
- BigInt (IR of Limba 2e8fbb8): a ref is a number of the run, counted
  as a str by every rule of the strings (deaths, edges, loads and stores,
  typed slots, retain and release); its functions run on mini-gmp (GMP
  6.3.0, in src/third_party/mini-gmp, unchanged, with its provenance),
  whose memory counts against the budget and gives NOMEM past it. The
  conversions to the reals are rounded once, straight to the type;
  big_cmp is -1, 0 or 1; the powers of 0, 1 and -1 are exact; a power
  that cannot fit is refused before it is made. test_vm checks the
  numbers alive at the end (; refs:).
- mini-gmp multiplies two limbs with one multiplication of the CPU
  (unsigned __int128, where the compiler has it), not four of their
  halves: the one change to its source, in PROVENANCE. pidigits with
  BigInt runs in about half the time.
- A calling convention of its own, for calls to C functions whose
  signature is known at run time, with no library for it: the System V
  AMD64 classification in C (src/vm/abi.c) and a trampoline of a few
  dozen lines of assembly that knows nothing of the rules. test_abi
  checks it against probes the C compiler lays out itself: integers of
  every width, reals, registers exhausted, structs of every class, a
  result in memory, the alignment of the stack. Not yet reachable from
  a program: the language has no external declarations yet.
- Calls of C libraries (IR version 5 of Limba ff801a9): a module of
  another platform (module.target) is refused; every library is opened
  and every symbol resolved before the first instruction, or nothing
  runs; call.ext goes through the calling convention of Meri (a struct
  by value as its bytes, a struct result written where the first operand
  says, _Bool for i1, the generation taken off every pointer the C
  sees); the C strings of runtime.def (cstr_*). meri --lib-path=DIR says
  where the libraries are; build.sh test runs Limba's probes against its
  library of known signatures, in every variant.
- IR version 6 (Limba fdd4f2d): an integer of 8, 16 or 32 bits of a C
  signature passes extended as its marker says (zext with zeros, sext
  with its sign). build.sh test runs the probes against libprobe built
  by gcc and by clang, whose callees rely on that extension.
- Registers from pieces of life: a value is alive, block by block, where
  liveness says, with holes between blocks, no longer one interval from
  its first point to its last. A value takes the register of a parameter
  it is passed to when their pieces allow it, even before that parameter
  has one; more loops end in LOOP and fewer copies are left on their
  edges. A jump to a JMP goes where that one goes.
- A range check, the sub of its lower limit in a register and the addr of
  that index are one instruction (`CHKADDRS`).
- The routines of arrays of Luxia `translate`, `reverse` and
  `occurrences` (functions `mem_translate`, `mem_reverse`, `mem_count` of
  the runtime), in C.
- `readbytes` and `writebytes` of Luxia (`io_read`, `io_write`): blocks of
  bytes from the input of `readline` and to the output of `write`, in
  order with them; the end of the input for good. The errors of input
  and output: a read that fails is the trap IO where it is, never the
  end; a write that fails is the trap IO without a position; a closed
  output ends the run with status 141 and no message (`SIGPIPE`
  ignored). `test_io` makes them happen.
- `tests/luxia/benchmarks/`: versions of the benchmarks that do where
  Python uses C what it does, for a comparison on equal terms.
  `pidigits-gmp`: GMP through external routines, as Python through
  ctypes; only Meri runs it (the reference interpreter does not call C).
  The others use the routines of Luxia, so the reference interpreter
  runs them and the net of the benchmarks compares them: `fasta-c` gathers
  its lines in a block of bytes for `writebytes`, as Python for
  `os.write`; `reverse-complement-c` reads the input whole by `readbytes`
  and turns each sequence by `translate` and `reverse`; `k-nucleotide-c`
  makes the codes by `translate` and counts the fragments of 1 and 2
  nucleotides by `occurrences`.
- `include/meri/meri.h`, Meri as a library: the compilation of a module
  whole or a function at a time, as the front end of Limba gives it
  (`meri_compile_begin`, `meri_compile_func`, `meri_compile_end`), its
  body never read again; `meri_link`; the end of a run flushed and
  reported as `meri` does (`meri_flush`, `meri_report`). The types of the
  slots released at a return are kept in the bytecode, no longer read
  from the body of the IR. `test_api` compiles Luxia through Limba both
  ways and expects the same run.
- Two faults found by the random net of prabanta: the end of a loop that
  no path reaches took into LOOP an add whose step was an immediate,
  with no register, and the compilation failed; `val` of an empty number
  (blanks, or a sign alone) read a sign never written, undefined
  behaviour. Each has its case in `tests/vm`.
- The constants of a function are found through two hash indexes (a
  value, and a pair of neighbours, each to the first place it has), no
  longer by a search from the start: a long block of constants, as a
  table written as an aggregate, compiled in quadratic time (10 000
  elements, 75 ms), now in linear time (4.6 ms). The bytecode is the same
  word for word.
- A function may have more than 65 536 constants: past them LOADK, TRAP,
  CHECK, CHKLIVE and CHKNL take a wide form, the index in a word of its
  own, and the range checks that keep their trap in 16 bits are not
  fused. The bytecode of a function with fewer constants is the same; a
  table of 30 000 elements, refused before, runs. `test_kwide`.
- Two tools for the checks, tracked: `tools/identity.sh` proves that a
  change leaves the bytecode as it was (the hash of the bytecode as bytes,
  `bytecode_dump`, and of the disassembly, before and after, on the same
  2 129 modules); `tools/sorvegliato.sh` runs every program of a test or
  measure with a time limit, a limit to its files and its process group
  killed at the end. The guard before it, kept outside the repository,
  could outlive a program and mark a later one as timed out.
- The repository: build script (it builds Limba first, in the same
  variant, and links every program with its library), licence, style.
