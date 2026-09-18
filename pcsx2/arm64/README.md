# ARM64 CPU backends

These backends are incomplete. Native instruction coverage should grow within
the existing provider and block-execution contracts, with differential tests
against the interpreters. Do not add game-specific execution shortcuts.

## EE

- `EERecompiler.cpp` owns the `R5900cpu` provider, block lookup, source validation,
  executable-memory lifetime and cycle accounting.
- `EECodeGenerator.cpp` translates supported integer, branch and RAM-access instructions
  into ARM64 code. It does not select CPU providers or run VM events.
- `Interpreter.h` exposes the shared EE execution driver. It owns boot hooks,
  event exits, exception recovery and fallback. The ordinary interpreter passes
  no backend; the ARM64 provider supplies a block executor. Single stepping and
  development-build debugger execution remain interpreted.

The generated function takes `cpuRegisters*` and returns a packed completed-prefix
count, exit action and branch target. Ordinary exits leave PC at the next
instruction and `code` at the last completed instruction. A zero-length exit
changes neither field. The provider charges the completed prefix in the same
fixed-point cycle units as the interpreter. Blocks contain no CP0 changes, so
the cycle scaling mode cannot change within one native block.

Scalar HI/LO instructions share the same emitter and exit contract as integer
ALU instructions: moves, signed/unsigned multiply and divide, and multiply-add,
including the second HI/LO bank. Each 32-bit arithmetic result is sign-extended
separately even for unsigned operations. Divide-by-zero and signed division
overflow retain EE results without a host exception. Multiply-add reconstructs
the accumulator from the low words of HI/LO and wraps at 64 bits; moves copy the
full 64 bits. These operations can also execute in nontrapping delay slots.
Packed MMI arithmetic and floating-point arithmetic remain outside this coverage.

Supported integer branches and jumps terminate the block. Their delay slot must
be a supported, nontrapping integer instruction in the same page and block.
Taken branches return after executing that slot, with the sequential PC still
just past it. The shared driver commits the target, applies the existing wait-loop
logic, commits cycles and tests events. Untaken branches preserve the interpreter's
boundaries: ordinary branches leave the delay slot for the next dispatch, likely
branches annul it, and BEQ/BNE and annulled likely branches request an event test
without committing cycles. Unsupported delay slots fall back with the entire
branch, before any link-register changes. Goemon TLB callbacks remain interpreted;
changing that gamefix invalidates the block cache. Branch/event integration still
needs proper testing across more games.

An unsupported opcode ends compilation. An unsupported memory access exits
before that instruction: MMIO, counter reads requiring event tests, unmapped
addresses and alignment errors are handled by the original interpreter path.
Ordinary mapped RAM loads and stores execute natively. A store overlapping the
current block's source exits after that store, before any stale instruction can
execute. This comparison uses host addresses to cover virtual aliases.

Blocks stay within one guest page. Every entry validates the current virtual
mapping and all compiled source words. This handles TLB remapping, DMA, code
patches and state restoration without requiring a separate invalidation scheme.
The initial implementation uses memory-backed guest registers; register caching
must preserve the same entry, exit and fallback contracts when introduced.

The cache is keyed by the full guest PC. Distinct blocks with the same page
offset coexist instead of repeatedly evicting and recompiling each other.
Rejected branch/delay pairs retain their source words so unchanged unsupported
pairs do not repeatedly invoke compilation. Patching either word rechecks the
pair. Cache hits validate bytes without decoding the entry opcode again;
allocation and compilation stay outside the frequently executed dispatcher.
Exhausting the reserved executable buffer resets the cache as a whole.

`R5900cpu::usesInterpreterExecution` specifies the shared driver's branch timing
and architectural TLB-miss behavior. It is independent of whether a provider
emits native instructions; the existing x86 recompiler keeps its own behavior.

## VU1

`VU1Recompiler.cpp` uses the interpreter's architectural registers and pipeline
queues. Unsupported pairs, branches and end-bit delay slots fall back as whole
instruction pairs. Pipeline retirement and XGKICK retain the reference timing.

Within a block, the first three pairs inspect the incoming FMAC queue. Later
pairs can use a dependency calculated during compilation: incoming four-cycle
FMAC results have matured, and only producers in the preceding three pairs can
still stall. Cycle-wrap boundaries retain the general queue scan. This does not
enable MTVU or adopt microVU's separate execution and synchronization protocol.

## Validation

`ee_recompiler_tests.cpp` compares complete CPU state, RAM, modified instruction
bytes and cycle accounting, including aliases and fallback exits. Branch tests
check targets, link/source aliases, delay-slot arithmetic, annulment, cycle
charges and the event actions requested from the shared driver. HI/LO tests cover
both banks, preserved register halves, input/output aliases, division edge cases,
accumulator wraparound, mixed-block dependencies and annulled delay slots.
`vu1_recompiler_tests.cpp` compares complete VU state and memory, including live
pipeline entries and execution-budget boundaries. Synthetic timing results are
kept under the ignored build directory; they are not game-performance guarantees.
