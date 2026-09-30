# ARM64 CPU backends

These backends are incomplete. Native instruction coverage should grow within
the existing provider and block-execution contracts, with differential tests
against the interpreters. Do not add game-specific execution shortcuts.

See [the investigation log](PERFORMANCE.md) for measured bottlenecks, the
limits of the current x64 comparison, and the reasoning behind each change.

Every processor now has a partial native block recompiler. Each one compiles
what it supports and hands the rest to the existing interpreter, one
instruction or pair at a time, through the same architectural state:

| Unit | Files | Setting | Still interpreted |
| --- | --- | --- | --- |
| EE | `EERecompiler.cpp`, `EECodeGenerator.cpp` | `EnableEE` | saturating and other MMI, COP1 DIV/SQRT/RSQRT and the ACC family, BC2, MMIO/unmapped accesses; COP2 ops other than FMAC, SYSCALL, CACHE and MTC0/ERET/EI/DI are interpreter handlers called from blocks |
| IOP | `IopRecompiler.cpp`, `IopCodeGenerator.cpp` | `EnableIOP` | J (idle loops are skipped natively), GTE, LWL/LWR/SWL/SWR, SYSCALL/BREAK, RFE, code outside the 8 MiB RAM window |
| VU0 micro mode | `VU0Recompiler.cpp`, `VU0Pipeline.cpp` | `EnableVU0` | JR/JALR/BAL, ISWR, MFP, RINIT/RGET/RNEXT/RXOR, the EFU pipe, E/M/D/T-bit pairs |
| VU1 | `VU1Recompiler.cpp`, `VU1Pipeline.cpp` | `EnableVU1` | ISWR, RINIT/RGET/RNEXT/RXOR, EATAN*/ESIN/EEXP, nested branches, end-bit delay slots |

VU0 macro mode (COP2 from EE code) runs its FMAC ops natively in EE blocks
and calls the interpreter's handlers for the rest. MTVU (`THREAD_VU1`) is supported. See
[Known issues](#known-issues) before relying on any of this.

## IOP

`IopRecompiler.cpp` owns the IOP block cache and the execution loop, and
`IopCodeGenerator.cpp` emits straight-line blocks of at most 32 instructions.
Generated code returns the completed-instruction prefix and an exit action,
following the EE contract below. Cycle accounting, event tests and exceptions
stay outside generated code. The outer loop copies `R3000AInterpreter.cpp`'s
`intExecuteBlock` cycle-budget loop, since that loop is not exported; keep the
two in sync. When no block can start at the current PC, the loop runs the
interpreter until a branch is actually taken, instead of looking up a block
again after every instruction.

Native code covers ALU, shift, multiply/divide, HI/LO moves, the COP0 moves
(not RFE), and aligned byte/halfword/word loads and stores through the LUT fast
path. Branches and jumps are native except plain J, whose interpreter handler
checks the delay slot for an IRX import-table marker. A delay slot must be a
supported non-memory, non-branch integer instruction. Stores honour
Status.IsC: with the cache isolated, the interpreter skips the RAM write, and
so does native code. Without that check, a normal boot never got past a black
screen, while resuming from a save state worked.

Only main RAM and its mirrors below 8 MiB are compiled. The IOP has no page
write protection, so each entry compares the whole block against its source
with `memcmp`. A word-indexed reverse map, modelled on the x86 recompiler's
`PSX_GETBLOCK` table, lets `Clear()` drop only the blocks a store overlaps;
`Clear()` runs on every IOP store. Clearing the whole cache on every store
was measured recompiling thousands of blocks per second. Self-modifying IRX
loaders and stores through a different RAM mirror still need proper testing.

The interpreter fallback keeps the earlier interpreter-side improvements.
Aligned instruction fetches in the first
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
and are eligible in native delay slots. Saturating arithmetic and other packed
operations remain interpreted.

COP1 (the EE FPU) covers MFC1/CFC1/MTC1/CTC1, LWC1/SWC1, ADD.S/SUB.S/MUL.S,
ABS.S/NEG.S/MOV.S, CVT.S.W/CVT.W.S, C.F/C.EQ/C.LT/C.LE and BC1F/BC1T/BC1FL/
BC1TL. Operands and results follow the interpreter's non-IEEE rules in
`FPU.cpp`: denormals flush to signed zero and Inf/NaN saturate to +-Fmax, with
the same O/U and sticky flags in FCR31. DIV.S/SQRT.S/RSQRT.S and the
ACC-based MADD/MSUB/MULA family still end the block. This needs proper
testing across games.

COP2 (VU0 macro-mode: `QMFC2`/`CFC2`/`QMTC2`/`CTC2` and the `COP2_SPECIAL`
arithmetic family) no longer ends the native block.
V{ADD,SUB,MUL,MADD,MSUB}[A][bc|i|q] are NEON code while VU0 is idle
(`VPU_STAT` bit 0 clear, when `COP2_SPECIAL`'s `_vu0FinishMicro()` does
nothing). They match `VUops.cpp`: `vuDouble()` on every input, VU0 overflow
clamping, per-lane MAC flags, then `VU_STAT_UPDATE` and `SYNCMSFLAGS`.
MADD/MSUB use fused `FMLA`/`FMLS`, because the interpreter is built with
`-ffp-contract=fast` and compiles them to `FMADD`/`FMSUB`. Changing
`vu0Overflow` or the Tri-Ace `VuAddSubHack` (which keeps VADDi on the
interpreter) resets the block cache.

Every other COP2 op, and any op while a microprogram runs, calls the exact
interpreter handler via `Blr` after setting `pc`/`code`. The call saves the
block's live registers: `x0`, `x1`, the GPR cache in `x2`-`x8`, `x14` and `lr`
(the block's own trailing `Ret()` otherwise returns into itself — see "COP2
(VU0 macro-mode) no longer ends the native EE block" in `PERFORMANCE.md`).
Only QMFC2/CFC2 write a GPR, and they drop just that one from the cache. BC2
(rs == 8) is not modeled and still ends the block. A VU0 micro-mode program started from macro mode runs on the VU0
provider described below.

SYSCALL, CACHE and COP0's MTC0/ERET/EI/DI call their interpreter handlers
from inside the block the same way, as `execI()` would: set `pc`/`code`, call,
charge the instruction's cycles with the rest of the block. This replaces a
dispatcher round trip, a failed block lookup and `execI()` per instruction;
SotC's kernel runs SYSCALL, MTC0 and ERET about 200k times a second each.
MTC0 (except Config), EI and DI only change COP0 state and schedule an event
test, so the block goes on with its cached GPRs. SYSCALL and ERET change `pc`,
MTC0 Config changes the cycle scale and CACHE can write memory back, so the
block ends after them and returns with the `pc` the handler left. Cycles are
charged at the scale the block was compiled with, which is what the
interpreter uses for an MTC0 Config itself.

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

A taken branch whose target is the entry PC of the block that just ran is
common in delay and polling loops. `TryExecute` runs that block again itself,
at most 4096 times, instead of returning to the shared driver. Between
iterations it does the driver's bookkeeping: it commits the branch
(`intFinishBranch` without the wait-loop hack, which is interpreter-only),
calls `intUpdateCPUCycles`, and checks `EEBranchEventDue`. When an event is due
it calls `intEventTest` and returns `Continue`, so the driver does not commit
the cycles a second time. Repeated iterations do not revalidate the block's
source; the iteration cap keeps that window short. As a result, `TryExecute`
can run more than one block per call and can process events itself. Two
older unit tests assume otherwise (see [Known issues](#known-issues)).

An unsupported opcode ends compilation. An unsupported memory access exits
before that instruction: MMIO, counter reads requiring event tests, unmapped
addresses and alignment errors are handled by the original interpreter path.
Ordinary mapped RAM loads and stores execute natively. A store overlapping the
current block's source exits after that store, before any stale instruction can
execute. This comparison uses host addresses to cover virtual aliases.

Blocks stay within one guest page. Every entry validates the current virtual
mapping. Compiling a block write-protects its source page through vtlb's
existing page tracking (`mmap_MarkCountedRamPage`), the mechanism the x86
recompiler uses. While the page stays protected (`ProtMode_Write`), or is not
RAM (`ProtMode_NotRequired`), entries skip comparing source words. A write to a
protected page faults, and `ClearProvider` drops every cached block. Pages that
keep self-modifying end up in `ProtMode_Manual` and are compared on every entry.
Such an untrusted block does the comparison itself: its generated code starts
by XORing its source with a copy of the words it was compiled from (8 bytes at
a time), and on a mismatch returns `NextBlock` without running anything. So
untrusted blocks can be linked and entered through `g_indirect` like trusted
ones; the dispatcher's `memcmp` then recompiles the block, and the recompile
bumps the link generation so no link leads to the old code. Needs proper
testing in games that really modify their code. The page tracking goes through the physical mapping, so it is used
only when that mapping and the virtual mapping resolve to the same byte;
otherwise the entry falls back to `memcmp`. The unit tests' synthetic code
buffers take that fallback. Guest GPRs live in `cpuRegs`. Within a block,
`GprCache` keeps the low 64 bits of up to seven of them in x2-x8 once an
integer op, address, store value or branch comparison has loaded or written
them. It is write-through: every write still stores to `cpuRegs`, so exits and
fallbacks see memory exactly as before, and a cached value is only dropped when
another emitter writes that GPR (`GPRWrite`: loads, HI/LO, MULT, packed and
COP1 transfers, branch links, QMFC2/CFC2).

Lookup goes through three levels. First, a one-entry cache holds the most
recently dispatched PC. Second, a 65536-entry direct-mapped cache is tagged with
the full virtual PC. Third, the block table is a fixed 2^19-slot,
linear-probed open-addressing table indexed by Fibonacci hashing. It replaced
`std::unordered_map`, whose prime bucket count cost an integer division per
probe; that division was measured as a large share of `TryExecute`'s own time.
Each table slot carries a generation tag. `Reset` and `ClearProvider` bump the
generation instead of wiping all slots, because a normal boot calls
`ClearProvider` on every TLB remap. With a full wipe per remap, a normal boot
appeared to hang. Blocks live in a `std::deque`, so pointers held by the caches
stay valid. `Compile` resets the whole cache once the table is three quarters
full. Unsupported entry opcodes are cached only while their mapped source
pointer and instruction word remain unchanged.

The cache is keyed by the full guest PC. Distinct blocks with the same page
offset coexist instead of repeatedly evicting and recompiling each other.
Rejected branch/delay pairs retain their source words so unchanged unsupported
pairs do not repeatedly invoke compilation. Patching either word rechecks the
pair. Cache hits do not decode the entry opcode again; on pages that are not
write-protected, they still compare the source bytes. Allocation and
compilation stay outside the frequently executed dispatcher. Exhausting the
reserved executable buffer resets the cache as a whole.

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

## VU0

`VU0Recompiler.cpp` and `VU0Pipeline.cpp` compile VU0 micro-mode programs.
They were adapted from the VU1 files below, which they leave untouched. VU0
runs synchronously on the EE thread and has no XGKICK path, so none of the
MTVU or packet machinery applies. Upper FMAC instructions use a copy of VU1's
decoder (currently identical) and emitters. The lower set covers
LQ/SQ/LQI/SQI/LQD/SQD, the integer ALU ops, MOVE/MR32/MFIR/MTIR,
ILW/ILWR/ISW, the CLIP/status/MAC flag family, DIV/SQRT/RSQRT/WAITQ, B and
the six integer-conditional branches. Up to eight
VF/ACC registers stay in host vector registers for the whole block. Every pair
goes through a generic preparation stub; VU0 has neither VU1's precomputed
schedule nor its deferred regions.

Traces follow a static B and the taken edge of an integer-conditional branch;
a guard exits on the fall-through edge. A trace ends at JR/JALR/BAL, at a
branch inside another branch's delay slot, and at a PC it has already visited,
since there is no loop-to-entry back edge yet. A block records every source
range it covered, and validation walks those ranges. An integer-conditional
branch uses a separate stub that waits for a pending ILW writing the tested
register.

DIV/SQRT/RSQRT/WAITQ stall on a pending divide and retire it *before* the
paired upper instruction runs. `VU0microInterp.cpp` runs `_vuTestFDIVStalls`
and `_vuTestPipes` ahead of `_vu0ExecUpper`, so a Q broadcast in the same pair
(MULq and friends) sees the newly retired value. The first version emitted the
stall in source order, and Ridge Racer V then rendered a large black shadow
over the car. The block-retire path also retires the FDIV and EFU slots. A
block that advances the cycle past a divide's latency would otherwise go on
reading the old Q. `Execute()` clears `VUFLAG_MFLAGSET` on entry, as the
interpreter does. `InvalidateAll()` also drops the pipeline stubs, since
rewinding the code buffer overwrites them.

JR/JALR/BAL, ISWR, MFP, the RINIT/RGET/RNEXT/RXOR random-number ops,
ESADD..WAITP and E/M/D/T-bit pairs fall back to the interpreter. On VU0,
unlike VU1, the M bit ends interpreter execution after its pair. This needs
proper testing beyond Ridge Racer V, particularly back-to-back branches.

## VU1

`VU1Recompiler.cpp` uses the interpreter's architectural registers and pipeline
queues. Static unconditional B edges can connect the branch, supported delay
pair and destination within one bounded native trace. Taken integer-branch
edges (IBEQ/IBNE/IBLTZ/IBGTZ/IBLEZ/IBGEZ) also connect their supported delay
pair and destination, including integer-load waits and VI backup selection.
Not-taken edges exit with complete architectural state. JR/JALR (target from
a VI register) and BAL execute natively together with their delay pair. The
trace then ends and the next dispatch resolves the target. A deferred region
ending in such a delay slot must not overwrite TPC with a compile-time value;
a version that did broke rendering. XTOP/XITOP read VIF1's TOP/ITOP, or the
MTVU thread's `vu1Thread.vifRegs` copy under MTVU, as `_vuXTOP`/`_vuXITOP`
do. Nested branches and end-bit delay slots retain interpreter fallback.
Pipeline retirement retains the interpreter timing. Normal XGKICK follows
microVU's delayed whole-packet policy; the interpreter and `XgKickHack` keep
incremental transfers.

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
still stall. A pair that reads no VF cannot stall on the FMAC pipe, so its
advance is one cycle even among the first three; this stops unknown ages from
spreading from the prologue into later scheduled pairs. Cycle-wrap boundaries
retain the general queue scan. The stubs publish TPC and `code` from two
adjacent `Instruction` fields (`tpc`, `code`) that `Compile()` fills in, with
one `Ldp`, instead of deriving them from pc/upper/lower on every pair.
Preparation entry points are specialized for read-free pairs, incoming
dependencies, each scheduled producer distance, and integer-branch VI
dependencies. `VU1Pipeline.cpp` emits these entry stubs and one shared ARM64
retirement body per code-cache generation. It mirrors the
reference retirement order in `VUPipeline.h`; generated blocks do not each contain
a copy of the retirement routine. Native retirement omits interpreter trace logs.
Five of the seven entry stubs always carry a zero integer-branch-wait register
and join the shared body past that check's dead `Cbz`; the two paths that can
still reach it with a real (or a rare wrap-fallback) value join earlier. See
"Skip the dead integer-branch wait check for non-branch entries" in
`PERFORMANCE.md`.

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
Pairs issued while a divide is still in the pipe stay on the precomputed
schedule. They are marked `fdiv_pending` and retire the FDIV slot inline,
after their own FMAC writeback, so the two status-flag merges keep
`VUPipeline::Retire`'s order. Deferred regions keep these pairs too, merging
the retired divide into the status flag the region keeps in w25. A divide
issue itself (DIV/SQRT/RSQRT) is scheduled, and deferrable, whenever its stall
on an earlier divide is known: the new entry is stamped with the cycle the
pair ends on, which is where a scheduled pair has already moved x26. It reads
the status scratch from memory for the sticky D/I bits it passes on, so a
region stores the latest flag op's scratch before a divide. Before this, every
divide split a region, and region exits were ~10% of SotC's VU1 thread. A pair that reads or
writes Q or P while an FDIV or EFU entry is outstanding stalls until that entry
retires, so its advance is not the nominal one. `AnalyzeRetirement` treats that
advance as unknown, and the following pairs use the generic path until their
producers' ages are known again. The EFU pipe's latency window is still
excluded from the schedule, as ILW's is for the IALU pipe. This needs proper
testing across games rather than only the differential tests.

WAITQ shares DIV/SQRT/RSQRT's pending-entry stall but issues nothing of its own,
so it leaves the FDIV pipe empty once retired instead of re-arming it. Its
`_VURegsNum` declares no reads or writes at all; without an explicit `VIwrite(Q)`
tag it would look like an ordinary fixed-timing, no-hazard pair and could be
scheduled or swept into a deferred region, silently skipping the runtime check
that actually settles Q. The interpreter also runs this stall-and-retire step
before executing the paired upper instruction, so an upper op that broadcasts Q
in the same pair (a common idiom pairing WAITQ with a Q-broadcast MULq/MADDq/etc.)
observes the freshly retired value; native emission orders the same-pair FDIV
stall ahead of the upper instruction to match. Both VU0 and VU1 do this for
all four FDIV-pipe ops (`IsFDIVPipe`), so a `MULq` + `DIV` pair reads the Q the
previous divide retires.

ESADD, ERSADD, ELENG, ERLENG, ESUM, ERCPR, ESQRT, ERSQRT and WAITP use a second,
structurally identical single-slot pipe (EFU, retiring into P instead of Q).
ESADD/ERSADD/ELENG/ERLENG reduce `fs.x^2+fs.y^2+fs.z^2` left to right before
diverging into a direct store, a reciprocal, a square root, or both; ESUM sums
all four lanes; ERCPR/ESQRT/ERSQRT read a single `Fs[fsf]` lane directly, unlike
the FDIV pipe's SQRT/RSQRT, which take `fabs()` first. None of the eight clamp
their output, matching the interpreter, and a `p >= 0` gate (ELENG/ERLENG/ESQRT/
ERSQRT) uses the AArch64 `lt` condition specifically, since it is true for both
a real negative operand and an unordered (NaN) one, correctly skipping the
square root in either case. ERCPR's reciprocal divides a `double` literal in
the interpreter (`1.0`), unlike every other reciprocal here (`1.0f`), so it
widens, divides and narrows instead of dividing directly in single precision.
WAITP mirrors WAITQ exactly, and needs the same manual `VIwrite(P)` tag as
WAITQ's `VIwrite(Q)` for the same reason. Unlike Q, P has no upper-instruction
broadcast source, so the same-pair emission-order fix WAITQ needed does not
apply here. `ClampInput`'s hardware-FZ shortcut (skip the explicit denormal
flush when arithmetic will do it anyway) does not hold for ERCPR/ESQRT/ERSQRT's
"leave the operand unchanged" branch, since no further arithmetic touches that
value there; `ClampInputAlways` covers that case explicitly. EATAN, EATANxy,
EATANxz, ESIN and EEXP (all evaluate a polynomial approximation, several of
them in `double` precision in the interpreter before narrowing to `float`)
still fall back. MFP is native.

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

MTVU (`THREAD_VU1`) is enabled, without adopting microVU's timing model
(`REC_VU1` stays off). As this section previously anticipated, the thread setting
alone was not enough: an explicit completion and interrupt handoff was required.
Unlike microVU, this backend delegates control flow to the shared VU1 interpreter,
which had no MTVU handling because that combination is unreachable on x64. On the
MTVU thread it therefore touched EE-owned state (VPU_STAT, FBRST, `vif1Regs`,
`cpuRegs.cycle`, INTC). Those sites now use the `VUFLAG_MTVURUNNING` VU1-local run
flag and report E/T bits through `mtvuInterrupts`, matching `mVUEBit`/`mVUTBit`.
See "MTVU (THREAD_VU1) on the ARM64 backend" in PERFORMANCE.md. Only Saru! Get
You! 2 and Ridge Racer V have been played with it so far; this needs proper
testing across games.

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
only the final MAC/non-sticky result is stored, unless the VU flag hack is on
(see "VU flag hack" below). This is a limited first step
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
Runtime profiles should distinguish these management costs from arithmetic throughput.

### Entry profiles and block linking

A block can be compiled for the exact pipeline state it is entered with (the
FMAC entries in flight and their ages, a pending divide, pending ILW results).
Such a *profiled* block knows its whole schedule from the first pair, so all of
it can be deferred. Execute() keeps up to several variants per entry PC and
picks the one whose profile matches. A loop whose back edge arrives in a
different state leaves through its end exit and links to the variant for that
state.

Every exit whose next PC is known links to the next block in generated code,
without returning to Execute(). A link slot is filled the first time its exit
returns to Execute(), and is taken only while the target is from the current
code generation, passed source validation since the last micro-memory write,
and Execute() would enter a block there (budget left, the program still
running, no pending branch, E-bit or XGKICK). An exit of a profiled block only
links to a profiled target, since its state follows from its own profile. A
slot keeps four targets, keyed by the TPC at the exit: a subroutine's JR returns
to each of its callers through the same exit. In SotC this took the returns to
Execute() from 2.25 to 1.26 per microprogram (the one left is the start).

The E-bit pair ends a trace. Execute() sees `VU1.ebit` and steps the delay slot
and the end of the program.

### VU flag hack

With `[EmuCore/Speedhacks] vuFlagHack` on (the default), a deferred region
computes no flags for an FMAC op whose MAC and status values nothing observes
(the `Dead` entry kind in `EmitDeferredRegion`). StoreMAC then only flushes
and clamps the result. Its sticky status bits (ZS/SS/US/OS) are dropped,
unless a status reader (FSAND/FSOR/FSEQ) later in the same region could see
them; those ops keep the exact `Raw` path. microVU's flag hack drops them the
same way (`sHackCond` in `mVUsetFlags`).

A region exit publishes the latest retired entry's flags, the live entries'
flags and the flag scratch only if the code after it can read them
(`ExitFlagsObserved`). The lookahead follows static paths from the exit for
up to 64 pairs. It relaxes the exit once a flag instruction's entry has
retired there (four pairs after it issued) before any MAC/status read,
FSSET or divide (these read the scratch), register branch or E/D/T bit.
Relaxed live entries publish zero flags, so nothing stale reaches the sticky
bits when they retire. The pairs the lookahead read are kept as guards and
validated with the block's own source, because they can lie outside the
trace. The clip flag is always exact. With the hack off, the region keeps
every sticky bit and publishes exact flags at every exit, matching the
interpreter exactly. The setting is part of `Options()`, so changing it
recompiles. Needs proper testing in games that read sticky flags across
microprograms.

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

`ee_recompiler_tests.cpp` also covers COP1 against `FPU.cpp`, COP2 transfers,
every native macro FMAC op (edge operands, masks, both clamp settings and
three FPCR modes; an unfused MADD fails it) and interpreter calls keeping
cached GPRs, and a self-looping branch against a manual replay of the
driver's bookkeeping. `iop_recompiler_tests.cpp` compares native IOP blocks with
the interpreter, including IsC stores. `vu0_recompiler_tests.cpp` checks that
native code is actually emitted: a recompiler that interprets every pair would
pass all the differential tests. It also covers back-to-back divides, the
DIV+MULq and WAITQ+MULq same-pair shapes, branches, and ILW before an integer
branch. The VU0 Q tests were confirmed to fail without their fixes. The
`VUFLAG_MFLAGSET` livelock and the EE block-table boot hang have no regression
tests. A test for a performance-only change cannot fail on the old, merely
slower code; a schedule dump with the change toggled confirmed that the VU1
prologue-scheduling test exercises it.

Differential tests are necessary but not sufficient. Several bugs on this
branch passed every test and showed up only in live play: whole-screen MTVU
breakage, the black shadow, the VU0 livelock, and two ways a normal boot never
started. Resuming a save state skips boot entirely, so check a normal boot as
well.

## Known issues

As of 2026-09-30, all 293 `core_test` tests pass.

- **Fused multiply-adds are pinned in the interpreter.** The recompilers emit
  FMADD/FMSUB/FMLA/FMLS for VU MADD/MSUB/OPMSUB and the ESADD family, and for
  COP1 MADDA/MSUBA, with fs as the (negated) first multiplicand. The
  interpreter used to rely on clang contracting `acc - fs * ft` the same way;
  the CI toolchain did not always, and even `std::fma` lets the compiler
  negate ft instead, which flips the sign of a NaN taken from fs. On ARM64,
  `VUops.cpp` and `FPU.cpp` now spell these out with inline assembly. This
  also fixed `SpecialFloatsAndChangedFloatingPointOptions`, which had failed
  since `4293623d6` on such NaN lanes.
- **The GS thread spins for MTVU.** `1dc082a1c` makes MTGS wait for MTVU
  packets with WFE before it sleeps. That was ~13% more fps in SotC, but the GS
  thread shows as ~70% busy in `sample` and `top` while it waits.
- Game coverage is narrow: Saru! Get You! 2 and 3, Ridge Racer V, Burnout 3 and
  Shadow of the Colossus, mostly from save states. Check a normal boot as well.
