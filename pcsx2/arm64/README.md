# ARM64 CPU backends

These backends are incomplete. Native instruction coverage should grow within
the existing provider and block-execution contracts, with differential tests
against the interpreters. Do not add game-specific execution shortcuts.

See [the intro performance investigation](PERFORMANCE.md) for measured bottlenecks
and the limits of the current x64 comparison.

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
Packed MMI integers use NEON for wrapping byte/halfword/word addition and
subtraction, signed greater-than/min/max, equality masks, 128-bit logic,
PEXT/PPAC lane rearrangement and PCPYLD/PCPYUD. Both source vectors are loaded
before the full destination is written, including aliased source/destination
registers. These non-saturating operations preserve host floating-point status
and are eligible in native delay slots. Saturating arithmetic, other packed
operations and floating-point arithmetic remain interpreted.

Supported integer branches and jumps terminate the block. Their delay slot must
be a supported, nontrapping integer instruction in the same page and block.
Taken branches return after executing that slot, with the sequential PC still
just past it. The shared driver commits the target, applies the existing wait-loop
logic, commits cycles and polls the event deadline. Untaken branches preserve
the interpreter's boundaries: ordinary branches leave the delay slot for the next dispatch, likely
branches annul it, and BEQ/BNE and annulled likely branches poll the event deadline
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

A small direct-mapped lookup cache avoids repeated hash-table searches for hot
PCs. Tags contain the full virtual PC. Unsupported entry opcodes are cached only
while their mapped source pointer and instruction word remain unchanged; rejected
branch/delay pairs still validate both words. Supported hits retain full source
validation. The owning map keeps block addresses stable across rehash, and reset
or shutdown clears lookup pointers before destroying blocks.

The cache is keyed by the full guest PC. Distinct blocks with the same page
offset coexist instead of repeatedly evicting and recompiling each other.
Rejected branch/delay pairs retain their source words so unchanged unsupported
pairs do not repeatedly invoke compilation. Patching either word rechecks the
pair. Cache hits validate bytes without decoding the entry opcode again;
allocation and compilation stay outside the frequently executed dispatcher.
Exhausting the reserved executable buffer resets the cache as a whole.

Native execution uses the x86 dispatcher's signed 64-bit event-deadline check
for branch polling, including interpreted branch fallbacks. Boot and ordinary
interpreter execution retain unconditional branch event tests. Explicit CP0 and
MMIO event tests remain forced; pending execution exits also bypass the deadline.
The active-backend flag is cleared on execution exit and on return to boot hooks.
This avoids synchronizing IOP and scanning device events at every short branch.
Interrupt-sensitive games still need broader testing.

`R5900cpu::usesInterpreterExecution` specifies the shared driver's branch timing
and architectural TLB-miss behavior. It is independent of whether a provider
emits native instructions; the existing x86 recompiler keeps its own behavior.

## VU1

`VU1Recompiler.cpp` uses the interpreter's architectural registers and pipeline
queues. Unsupported pairs, branches and end-bit delay slots fall back as whole
instruction pairs. Pipeline retirement retains the interpreter timing. Normal
XGKICK follows microVU's delayed whole-packet policy; the interpreter and
`XgKickHack` keep incremental transfers.

Within a block, the first three pairs inspect the incoming FMAC queue. Later
pairs can use a dependency calculated during compilation: incoming four-cycle
FMAC results have matured, and only producers in the preceding three pairs can
still stall. Cycle-wrap boundaries retain the general queue scan. Preparation
entry points are specialized for read-free pairs, incoming dependencies,
and each scheduled producer distance. `VU1Pipeline.cpp` emits these entry stubs
and one shared ARM64 retirement body per code-cache generation. It mirrors the
reference retirement order in `VUPipeline.h`; generated blocks do not each contain
a copy of the retirement routine. Native retirement omits interpreter trace logs.

Each block assigns up to eight frequently accessed VF/ACC registers to q8..q15.
The assignment is fixed for the block, including every budget exit. Entry loads
these values; exit publishes them before returning to the shared driver or
interpreter. The generated preparation routine preserves full cached vectors
through its private ABI. Actual XGKICK transfers publish the cache, call the
original C++ transfer routine, then reload it; this also handles AAPCS64's
caller-clobbered upper vector halves. Credit-only XGKICK ticks stay native.

This does not enable MTVU or adopt microVU's separate execution and synchronization
protocol. ARM64's current fallback still updates EE/VIF state and interrupts
synchronously. Moving it to the MTVU worker requires an explicit completion and
interrupt handoff, not just enabling the thread setting.

Arithmetic input clamping uses signed/unsigned NEON min operations to clamp both
signs of infinity/NaN. When VU1's FPCR flushes denormals, arithmetic supplies the
input flush directly; other modes retain explicit signed-zero conversion. The
code-cache options include this FPCR setting so changing it recompiles affected
blocks. Output overflow clamping uses the same signed/unsigned min technique.
FZ arithmetic already produces signed zero for tiny results, so that mode omits
redundant software underflow classification and conversion. Other FPCR modes
retain them, with the same MAC/status results as the interpreter.

### microVU reference and remaining execution costs

The x86 backend provides architectural references beyond instruction selection:

- `x86/microVU_Analyze.inl` and `microVU_Compile.inl` (`mVUincCycles`,
  `mVUsetCycles`) compute lane dependencies and stalls during compilation.
  `microRegInfo` carries pipeline state between compiled blocks. ARM64 still
  retires architectural pipeline queues at runtime for each instruction pair.
- `x86/microVU_Flags.inl` (`mVUsetFlags`) selects flag instances and necessary
  updates, including flags needed by following blocks. Eliminating an ARM64
  update requires preserving the state visible at budget exits and fallback,
  not merely finding no flag reader in the current block.
- `x86/microVU_Lower.inl` (`mVU_XGKICK_`) normally transfers a complete packet
  at the scheduled kick. The separate `CHECK_XGKICKHACK` path accumulates cycles
  and synchronizes at memory writes and block boundaries. ARM64 now shares the
  whole-packet copy helper with microVU for ordinary native execution. The
  cycle-based interpreter path remains available for `XgKickHack`.

These are different execution contracts, not just different SIMD encodings.
The native packet policy keeps a pending kick in the existing architectural
XGKICK fields. It completes after the following instruction pair, including its
lower store, as microVU does. A pending kick limits the next native block call
to one pair; block exit publishes the complete architectural state before GIF
callbacks. Interpreter fallback observes the same boundary, while a second
XGKICK flushes the old request and starts a new delay. A pipeline stall must not
cause the transfer to move before the following store.

Cycle-budget exits retain an unexecuted delay, and completion clears the busy
bit and wakes VIF when required. Forced completion does not charge per-qword VU
cycles in packet mode. Restored incremental packets keep their mode through all tags. Bit 1 of the
existing enable word marks a native packet request; bit 0 remains the legacy
enable bit, so old states and pending native delays remain distinguishable. Packet copies use the
existing GIF arbitration and 16 KiB wrap handling, including buffering when the
GIF cannot run. Save-state layouts are unchanged. This adopts microVU's ordinary
transfer policy, not cycle-exact GIF timing; wider game coverage is still needed.

FMAC/FDIV/EFU/IALU queues and arithmetic flags remain interpreter-compatible.
For sufficiently long blocks, the compiler now tracks FMAC ages and known stalls.
After a generic prefix drains incoming entries, it emits known retirement counts
instead of checking every queue at every pair. An entry guard rejects pending
special pipelines or XGKICK, irregular incoming queues and cycle wrap. Unknown
timing keeps the generic preparation path. Every queue entry remains materialized,
including at budget exits; sticky flags include all retired entries even when
only the final MAC/non-sticky result is stored. This is a limited first step
toward compiler scheduling, not cross-block pipeline or flag-liveness analysis.
Runtime profiles should distinguish these management costs from arithmetic throughput.

## Validation

`ee_recompiler_tests.cpp` compares complete CPU state, RAM, modified instruction
bytes and cycle accounting, including aliases and fallback exits. Branch tests
check targets, link/source aliases, delay-slot arithmetic, annulment, cycle
charges and the event actions requested from the shared driver. HI/LO tests cover
both banks, preserved register halves, input/output aliases, division edge cases,
accumulator wraparound, mixed-block dependencies and annulled delay slots.
Packed-integer tests compare complete CPU state across edge/random lane values,
all source/destination alias patterns, dependent instruction blocks, quadword
transfers and delay slots. They also check unchanged host FPSR and fallback for
unsupported packed selectors. Broader game coverage still needs proper testing.
`vu1_recompiler_tests.cpp` compares complete VU state and memory, including live
pipeline entries and execution-budget boundaries, including overlapping FMAC,
FDIV, EFU and IALU retirement and cycle wrap. Register-cache tests cover partial
writes, ACC, paired upper/lower hazards and exits at every instruction prefix.
A generated wrapper checks all preparation entries across C++ callouts, including
cached-value publication, callback modifications and upper-vector ABI clobbers.
Synthetic timing results are kept under the ignored build directory; they are
not game-performance guarantees.
