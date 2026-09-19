# ARM64 CPU backends

These backends are incomplete. Native instruction coverage should grow within
the existing provider and block-execution contracts, with differential tests
against the interpreters. Do not add game-specific execution shortcuts.

See [the intro performance investigation](PERFORMANCE.md) for measured bottlenecks
and the limits of the current x64 comparison.

## IOP

ARM64 still uses the IOP interpreter. Aligned instruction fetches in the first
8 MiB physical RAM window read through the live RLUT directly, including RAM
mirrors and virtual aliases. Each fetch reloads both the page mapping and the
instruction; no decoded-code cache or invalidation mechanism is introduced.
ROM, unmapped memory and hardware regions retain `iopMemRead32` handling. The
same path serves the J instruction's import-table delay-slot probe. Instruction
execution, branch timing, event polling and debugging hooks remain unchanged.
`R3000AInterpreter.h` resolves grouped opcodes to their existing leaf handlers
inside the execution driver, avoiding additional indirect dispatch through
SPECIAL, REGIMM, COP0 and COP2 grouping functions. Exact NOP skips its empty
handler after the driver has performed the usual PC/cycle/debug work. Opcode
semantics remain in the existing tables; no extra decoded-code cache is used.
At taken branch boundaries, scheduled device/counter work is scanned only when
`iopNextEventCycle` is due, using the x64 dispatcher's signed 64-bit comparison.
Already enabled hardware interrupts still force a scan immediately, including
when CP0 Status changes in the delay slot. The EE-side unconditional event test
and all event scheduling APIs remain in place. This avoids repeatedly walking
the event queues during short loops without fast-forwarding guest cycles.
These shared interpreter changes still need proper testing in PS1 gameplay and
on other host architectures.

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
queues. Static unconditional B edges can connect the branch, supported delay
pair and destination within one bounded native trace. Taken IBEQ/IBNE/IBGTZ edges also
connect their supported delay pair and destination, including integer-load waits
and VI backup selection. Not-taken edges exit with complete architectural state. Other conditional/indirect branches,
nested branches and end-bit delay slots retain interpreter fallback. Pipeline
retirement retains the interpreter timing. Normal
XGKICK follows microVU's delayed whole-packet policy; the interpreter and
`XgKickHack` keep incremental transfers.

Connected regions share the VF/ACC cache assignment and the pipeline schedule in
execution order. They do not publish/reload cached vectors or restart preparation
at an internal B or taken integer-branch edge. Before entry, source validation checks each range the remaining cycle budget
can reach, including destination edits. Each pair advances at least one cycle;
bytes beyond that bound are checked on a later call before they can execute. Per-pair budget exits publish the correct branch,
delay and TPC state; a complete deferred region publishes its final state as
before. A back edge to the same trace entry can stay native when no branch delay
is unresolved. It retains the host frame and VF/ACC cache, rechecks the budget
and pipeline guard, and reenters the generic incoming-state preparation. Back
edges to other internal positions still end the trace. The trace is bounded by
the same 256-pair limit. Connecting not-taken successors and independently cached target-state matching remain
future work.
Wider gameplay and callback combinations still need proper testing.

Within a block, the first three pairs inspect the incoming FMAC queue. Later
pairs can use a dependency calculated during compilation: incoming four-cycle
FMAC results have matured, and only producers in the preceding three pairs can
still stall. Cycle-wrap boundaries retain the general queue scan. Preparation
entry points are specialized for read-free pairs, incoming dependencies,
each scheduled producer distance, and integer-branch VI dependencies. `VU1Pipeline.cpp` emits these entry stubs
and one shared ARM64 retirement body per code-cache generation. It mirrors the
reference retirement order in `VUPipeline.h`; generated blocks do not each contain
a copy of the retirement routine. Native retirement omits interpreter trace logs.

LQI/SQI and LQD/SQD execute vector transfers and VI address updates natively.
They wrap data-memory addresses at 16 KiB and VI updates at 16 bits, preserving
upper VI bits, component masks and the interpreter's encoded-register guards.
The VI backup is created even when VF0, VI0 or a zero mask suppresses a transfer
or update. Upper/lower destination conflicts still suppress the whole lower
operation, including its address update. FMAC issue and VI backup retirement
use the existing shared pipeline machinery, including deferred regions.

CLIP uses NEON signed integer comparisons and a weighted reduction for its six
XYZ tests, preserving the interpreter's denormal-W threshold and 24-bit history.
FCAND/FCEQ/FCOR read the retired architectural CLIP flag and write only VI1's low
halfword. They do not create an arithmetic VI backup. Retirement of CLIP producers
and reads of architectural flags use the generic path; deferred regions end before
these observations so pending flag snapshots remain visible at the correct cycle.
This retains the shared pipeline design rather than adding a separate flag timeline.

DIV, SQRT and RSQRT compute with the interpreter's operand and result clamping,
including the denormal flush and the optional overflow clamp. A zero divisor (DIV)
or negative operand (SQRT/RSQRT) produces the signed maximum float or signed zero
and sets the I or D status bit, matching `_vuDIV`/`_vuSQRT`/`_vuRSQRT`; NaN operands
use the N-flag-only ARM64 condition codes so an unordered compare stays
false-for-NaN like the plain C comparisons they mirror. Each result also lands in
the staging Q field the interpreter writes. Issue stages Q into the shared
single-slot FDIV pipe for its 7 (DIV/SQRT) or 13 (RSQRT) cycle latency, where the
existing generic retirement publishes it. An outstanding entry stalls the next
FDIV issue and is retired before being replaced, mirroring `_vuTestFDIVStalls`
followed by `_vuTestPipes`. FDIV reads also participate in the FMAC hazard scan.
Because a deferred region skips shared preparation, pairs within the pipe's
latency stay on the generic path, as ILW already does for the IALU pipe. The EFU
instructions still fall back. This needs proper testing across games rather than
only the differential tests.

WAITQ shares DIV/SQRT/RSQRT's pending-entry stall but issues nothing of its own,
so it leaves the FDIV pipe empty once retired instead of re-arming it. Its
`_VURegsNum` declares no reads or writes at all; without an explicit `VIwrite(Q)`
tag it would look like an ordinary fixed-timing, no-hazard pair and could be
scheduled or swept into a deferred region, silently skipping the runtime check
that actually settles Q. The interpreter also runs this stall-and-retire step
before executing the paired upper instruction, so an upper op that broadcasts Q
in the same pair (a common idiom pairing WAITQ with a Q-broadcast MULq/MADDq/etc.)
observes the freshly retired value; native emission orders the same-pair FDIV
stall ahead of the upper instruction to match.

IBLTZ, IBLEZ and IBGEZ reuse the existing integer-branch path, including the VI
backup lookup and the combined FMAC/IALU waits, and differ only in the condition
that skips the taken edge.

ILWR is ILW without the immediate offset: VI[Is] is already the quadword index,
so it shares ILW's IALU pipe timing and lane-priority selection exactly.

FCSET, FCGET, FSEQ, FSSET, FSAND, FSOR, FMEQ, FMAND and FMOR extend FCAND/FCEQ/
FCOR's pattern to the status and MAC flag instances: read a retired flag,
combine it with an encoded immediate or VI[Is], and write VI[It]'s low halfword,
or (FCSET/FSSET) write a staging flag field for the existing FMAC-pipe
retirement to publish. Needs proper testing across games.

ISW writes each masked lane independently (X/Y/Z/W are separate destination
addresses, unlike ILW's single priority-selected lane), so it does not skip
when It == 0. It broadcasts VI[It] into all four lanes and reuses the existing
StoreMasked helper.

ILW reads the low halfword of the final selected component, wraps VU1 data memory,
and preserves the upper half of the VI register. It issues the same four-cycle
IALU entry even for a masked-out or VI0 destination, without creating an arithmetic
VI backup. ILW clears schedule readiness and keeps the following four pairs on
generic retirement; readiness is checked again when static scheduling resumes.
Each sufficiently long region with a known schedule can defer queue construction.
It publishes its queues and checks the remaining budget before returning to the
ordinary generated path, keeping VF/ACC cached across branch preparation. Branch
preparation combines upper FMAC stalls with matching IALU waits, then retires
pipelines before testing the signed VI value or its applicable backup. Since
integer waits can change FMAC ages, analysis forgets uncertain ages at integer branches
and resumes static retirement only when subsequent pairs establish known timing.

Each block assigns up to eight frequently accessed VF/ACC registers to q8..q15.
The assignment is fixed for the block, including every budget exit. Entry loads
these values; exit publishes them before returning to the shared driver or
interpreter. Host d8..d15 preservation saves only the used cache registers,
rounded up to an even count for paired stores and stack alignment. Unused host
registers remain untouched; guest VF/ACC publication is unchanged. The generated preparation routine preserves full cached vectors
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
  `microRegInfo` carries pipeline state between compiled blocks. ARM64 now
  defers queue materialization inside fully budgeted suffixes, but still
  publishes complete architectural state at every block boundary.
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
XGKICK fields. Ordinary ARM64 XGKICK issuance is native: it flushes an older
request if present, reads the low VI address and initializes the same transfer
fields and VU0 busy bit as the reference. XgKickHack keeps interpreter issuance,
and changing that policy invalidates compiled code. A request completes after
the following instruction pair, including its
lower store, as microVU does. The next native call carries a pending-packet flag.
After its first pair commits, a shared private-ABI entry publishes VF/ACC, calls
the existing packet transfer and reloads the cache. Execution then continues in
the same native frame if budget remains. Interpreter fallback observes the same
transfer boundary. The same completion entry handles an in-trace delayed pair;
a second XGKICK flushes the old request and starts a new delay. Kick issuance
and delayed completion stay outside deferred queue regions, and their callbacks
reset timing analysis before known scheduling resumes. A pipeline stall must not
cause the transfer to move before the following store.

Cycle-budget exits retain an unexecuted delay, and completion clears the busy
bit and wakes VIF when required. Forced completion does not charge per-qword VU
cycles in packet mode. Restored incremental packets keep their mode through all tags. Bit 1 of the
existing enable word marks a native packet request; bit 0 remains the legacy
enable bit, so old states and pending native delays remain distinguishable. Packet copies use the
existing GIF arbitration and 16 KiB wrap handling, including buffering when the
GIF cannot run. Save-state layouts are unchanged. This adopts microVU's ordinary
transfer policy, not cycle-exact GIF timing; wider game coverage is still needed.

MAC generation weights each enabled NEON lane by its architectural bit position
before reducing zero/sign/underflow/overflow groups with a horizontal sum. This
replaces per-lane vector-to-integer extraction. Disabled lanes contribute zero,
and the four flag groups do not overlap or carry into one another. Input/output
clamping and the reference zero test are unchanged.

FMAC/FDIV/EFU/IALU queues and arithmetic flags remain interpreter-compatible.
For sufficiently long blocks, the compiler now tracks FMAC ages and known stalls.
After a generic prefix drains incoming entries, it emits known retirement counts
instead of checking every queue at every pair. An entry guard rejects pending
XGKICK, irregular incoming FMAC queues and cycle wrap. After the explicit
packet completion, the full guard runs again against the current
state; the generic prefix still drains incoming work before scheduled execution. Pending FDIV,
EFU and IALU work initially keeps the generic path, but readiness is checked again
at the first scheduled pair and at the deferred region boundary. Once these
queues drain, the validated block may use scheduled execution. ILW issues integer work and clears readiness; integer branches wait for matching loads
and breaks the static schedule. Other supported integer operations have zero
pipeline latency. Unknown timing keeps the
generic preparation path. Every queue entry is materialized
at observable exits; sticky flags include all retired entries even when
only the final MAC/non-sticky result is stored. This is a limited first step
toward compiler scheduling, not cross-block pipeline or flag-liveness analysis.
The generated block retains the current cycle in x26. Scheduled preparation
updates it directly; queue insertion and budget checks consume it without
reloading architectural memory. Before generic preparation after a scheduled
pair, and at every block exit, it is published to VURegs. Generic preparation
reloads it afterwards so callback changes remain visible. x26 is saved/restored
by the block and preserved by the private pipeline ABI.
Each contiguous scheduled region of at least eight pairs has a second execution
path. It checks the budget for that region once before entering it.
Only the validated, callback-free case can enter; partial budgets and pending
special pipelines keep the existing per-pair path. q28..q31 retain the four
FMAC flag snapshots relative to the entry write position. x25/x28 retain retired
STATUS/MAC values. Producer metadata and issue-cycle offsets are compiler facts,
so the path restores only each slot's last writer at exit, including overwritten
inactive entries and padding. It then publishes queue indices/count, flags,
TPC, code and cycles. VF/ACC publication uses the common exit. VI backup timing
and all arithmetic execute in their original order. The VI backup countdown is
accumulated until the next VI write or region exit, with byte saturation; this
preserves the value seen by BackupVI without updating memory every pair.

Compiled traces contain at most 256 pairs, with every source range bounded by
micro-memory and supported instructions. Source validation covers every pair that the current budget can reach. Generated-code space
and cycle-wrap protection scale with this limit. This reduces artificial block
boundaries without adding cross-block linking. A 512-pair limit did not improve
the measured opening-movie workload; see PERFORMANCE.md for the comparison.
MaxBlockBytes is the pre-compilation free-space threshold, not an allocation
reserved for every block.

This removes per-pair queue construction, retirement memory traffic and budget/
TPC/code updates from those regions. It shares pair emission and metadata encoding
with the general path. EmitPair must preserve q28..q31 and x25..x28. There are no
C++ callbacks or unsupported instructions inside a deferred region; expanding supported
operations must preserve that invariant. Wider gameplay still needs proper testing.
This is an internal block-state contract, not yet cross-block linking or MTVU.
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
